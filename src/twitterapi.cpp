/*
    Copyright (C) 2017-19 Sebastian J. Wolf
                  2020 Mirian Margiani

    This file is part of Piepmatz.

    Piepmatz is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Piepmatz is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Piepmatz. If not, see <http://www.gnu.org/licenses/>.
*/
#include "twitterapi.h"

#include "imageresponsehandler.h"
#include "imagemetadataresponsehandler.h"
#include "downloadresponsehandler.h"
#include "tweetconversationhandler.h"
#include "contentextractor.h"
#include "QGumboParser/qgumbodocument.h"
#include "QGumboParser/qgumbonode.h"
//#include <QOverload> // only available since Qt 5.7
#include <QBuffer>
#include <QFile>
#include <QHttpMultiPart>
#include <QXmlStreamReader>
#include <QProcess>
#include <QTextCodec>
#include <QRegularExpression>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>

// This macro takes the callers name (as: TwitterApi::helpPrivacy) and converts
// it to three arguments for TwitterApi::genericRequest: title, successSignal, errorSignal
// Example: STANDARD_REQ(TwitterApi::helpPrivacy) expands to
// QString("TwitterApi::helpPrivacy"), TwitterApi::helpPrivacySuccessful, TwitterApi::helpPrivacyError
// This helps in code reduction and is to avoid typo-bugs.
#ifndef STANDARD_REQ
#define STANDARD_REQ(name) QString((#name)), &name##Successful, &name##Error
#endif

//TwitterApi::TwitterApi(O1Requestor* requestor, QNetworkAccessManager *manager, Wagnis *wagnis, QObject* parent) : QObject(parent) {
TwitterApi::TwitterApi(O1Requestor* requestor, QNetworkAccessManager *manager, O1Requestor *secretIdentityRequestor, QObject* parent) : QObject(parent) {
    this->requestor = requestor;
    this->manager = manager;
    this->secretIdentityRequestor = secretIdentityRequestor;
    //this->wagnis = wagnis;
}

template<typename SIG_SUCCESS_T, typename SIG_FAILURE_T>
QNetworkReply *TwitterApi::genericRequest(const QString &apiCall, const QString &title,
                                          SIG_SUCCESS_T successSignal, SIG_FAILURE_T errorSignal,
                                          bool isGetRequest, TwitterApi::ParametersList parameters, bool includeQueryParameters,
                                          ApiFinishedHandler<SIG_SUCCESS_T, SIG_FAILURE_T> finishedHandler,
                                          ApiFailureHandler<SIG_FAILURE_T> errorHandler,
                                          bool useSecretIdentity)
{
    qDebug() << QString("generic request (%1):").arg(isGetRequest ? "get" : "post") << title << parameters;
    QNetworkReply *reply = runRawRequest(apiCall, isGetRequest, parameters, includeQueryParameters, useSecretIdentity);

    if (errorHandler != nullptr) {
        // QNetworkReply::error is a signal has an overloaded method. Before Qt 5.7,
        // we have to manually select which one to use as below. Starting with Qt 5.7,
        // there is 'QOverload<void>::of(...)' to help with that.
        connect(reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                this, [=](const QNetworkReply::NetworkError& error) {
            (this->*errorHandler)(title, reply, error, errorSignal);
        });
    }

    if (finishedHandler != nullptr) {
        connect(reply, &QNetworkReply::finished, this, [=]() {
            (this->*finishedHandler)(title, reply, successSignal, errorSignal);
        });
    }

    return reply;
}

QNetworkReply *TwitterApi::runRawRequest(const QString &apiCall, bool isGetRequest,
                                         const TwitterApi::ParametersList &parameters, bool includeQueryParameters,
                                         bool useSecretIdentity)
{
    QUrl url = QUrl(apiCall);

    if (includeQueryParameters) {
        QUrlQuery urlQuery = QUrlQuery();
        for (QString key : parameters.keys()) {
            urlQuery.addQueryItem(key, QString(QUrl::toPercentEncoding(parameters[key])));
        }
        url.setQuery(urlQuery);
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> preparedParameters;
    for (QString key : parameters.keys()) {
        preparedParameters.append(O0RequestParameter(key.toUtf8(), parameters[key].toUtf8()));
    }

    O1Requestor* usedRequestor = requestor;
    if (useSecretIdentity && secretIdentityRequestor != nullptr) {
        usedRequestor = secretIdentityRequestor;
    }

    QNetworkReply *reply;
    if (isGetRequest) {
        reply = usedRequestor->get(request, preparedParameters);
    } else {
        QByteArray postData = O1::createQueryParameters(preparedParameters);
        reply = usedRequestor->post(request, preparedParameters, postData);
    }

    return reply;
}

void TwitterApi::genericHandlerFinished(const QString &title, QNetworkReply *reply,
                                        ApiResultMap successSignal, ApiResultError errorSignal)
{
    qDebug() << "generic finished (map):" << title <<
                QString(reply->request().hasRawHeader(HEADER_NO_RECURSION) ?
                    "(probably a secret identity response)" : "(standard response)");
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        emit (this->*successSignal)(jsonDocument.object().toVariantMap());
    } else {
        emit (this->*errorSignal)(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::genericHandlerFinished(const QString &title, QNetworkReply *reply,
                                        ApiResultList successSignal, ApiResultError errorSignal)
{
    qDebug() << "generic finished (list):" << title <<
                QString(reply->request().hasRawHeader(HEADER_NO_RECURSION) ?
                    "(probably a secret identity response)" : "(standard response)");
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        emit (this->*successSignal)(jsonDocument.array().toVariantList());
    } else {
        emit (this->*errorSignal)(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::genericHandlerFailure(const QString& title, QNetworkReply* reply,
                                       QNetworkReply::NetworkError errorCode,
                                       TwitterApi::ApiResultError errorSignal)
{
    qWarning() << "generic failure:" << title << (int)errorCode << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit (this->*errorSignal)(parsedErrorResponse.value("message").toString());
}

void TwitterApi::verifyCredentials()
{
//    if (!wagnis->hasFeature("contribution") && wagnis->getRemainingTime() == 0) {
//        emit verifyCredentialsError("You haven't completed the registration process!");
//        return;
//    }
    genericRequest(API_ACCOUNT_VERIFY_CREDENTIALS, STANDARD_REQ(TwitterApi::verifyCredentials));
}

void TwitterApi::accountSettings()
{
    genericRequest(API_ACCOUNT_SETTINGS, STANDARD_REQ(TwitterApi::accountSettings));
}

void TwitterApi::helpConfiguration()
{
    genericRequest(API_HELP_CONFIGURATION, STANDARD_REQ(TwitterApi::helpConfiguration));
}

void TwitterApi::helpPrivacy()
{
    genericRequest(API_HELP_PRIVACY, STANDARD_REQ(TwitterApi::helpPrivacy));
}

void TwitterApi::helpTos()
{
    genericRequest(API_HELP_TOS, STANDARD_REQ(TwitterApi::helpTos));
}

void TwitterApi::postTweetRequest(const QString &title, TwitterApi::ParametersList parameters)
{
    qDebug() << "post tweet" << title << parameters["place_id"] << parameters["media_ids"]
             << parameters["attachment_url"] << parameters["in_reply_to_status_id"];
    genericRequest(API_STATUSES_UPDATE, STANDARD_REQ(TwitterApi::tweet), false, parameters);
}

void TwitterApi::tweet(const QString &text, const QString &placeId)
{
    ParametersList params;
    params["status"] = text;
    if (!placeId.isEmpty()) params["place_id"] = placeId;
    postTweetRequest("TwitterApi::tweet", params);
}

void TwitterApi::replyToTweet(const QString &text, const QString &replyToStatusId, const QString &placeId)
{
    ParametersList params;
    params["status"] = text;
    params["in_reply_to_status_id"] = replyToStatusId;
    params["auto_populate_reply_metadata"] = "true";
    if (!placeId.isEmpty()) params["place_id"] = placeId;
    postTweetRequest("TwitterApi::replyToTweet", params);
}

void TwitterApi::retweetWithComment(const QString &text, const QString &attachmentUrl, const QString &placeId)
{
    ParametersList params;
    params["status"] = text;
    params["attachment_url"] = attachmentUrl;
    if (!placeId.isEmpty()) params["place_id"] = placeId;
    postTweetRequest("TwitterApi::retweetWithComment", params);
}

void TwitterApi::tweetWithImages(const QString &text, const QString &mediaIds, const QString &placeId)
{
    ParametersList params;
    params["status"] = text;
    params["media_ids"] = mediaIds;
    if (!placeId.isEmpty()) params["place_id"] = placeId;
    postTweetRequest("TwitterApi::tweetWithImages", params);
}

void TwitterApi::replyToTweetWithImages(const QString &text, const QString &replyToStatusId, const QString &mediaIds, const QString &placeId)
{
    ParametersList params;
    params["status"] = text;
    params["in_reply_to_status_id"] = replyToStatusId;
    params["auto_populate_reply_metadata"] = "true";
    params["media_ids"] = mediaIds;
    if (!placeId.isEmpty()) params["place_id"] = placeId;
    postTweetRequest("TwitterApi::replyToTweetWithImages", params);
}

void TwitterApi::homeTimeline(const QString &maxId)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["exclude_replies"] = "false";
    if (!maxId.isEmpty()) params["max_id"] = maxId;
    params["count"] = "200";
    params["include_ext_alt_text"] = "true";

    auto finishedHandler = &TwitterApi::handleHomeTimelineFinished;
    if (!maxId.isEmpty()) finishedHandler = &TwitterApi::handleHomeTimelineLoadMoreFinished;

    genericRequest<ApiResultList>(API_STATUSES_HOME_TIMELINE, "TwitterApi::homeTimeline",
                   nullptr, &TwitterApi::homeTimelineError,
                   true, params, true, finishedHandler, &TwitterApi::genericHandlerFailure);
}

void TwitterApi::mentionsTimeline()
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["include_entities"] = "true";
    params["count"] = "200";
    params["include_ext_alt_text"] = "true";
    genericRequest(API_STATUSES_MENTIONS_TIMELINE, STANDARD_REQ(TwitterApi::mentionsTimeline), true, params, true);
}

void TwitterApi::retweetTimeline()
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["include_entities"] = "true";
    params["trim_user"] = "false";
    params["count"] = "10";
    params["include_ext_alt_text"] = "true";
    genericRequest(API_STATUSES_RETWEET_TIMELINE, STANDARD_REQ(TwitterApi::retweetTimeline), true, params, true);
}

void TwitterApi::showStatus(const QString &statusId, const bool &useSecretIdentity)
{
    // Very weird, some statusIds contain a query string. Why?
    QString sanitizedStatus = statusId;
    int qm = statusId.indexOf(QLatin1Char('?'));
    if (qm >= 0) sanitizedStatus = statusId.left(qm);

    ParametersList params;
    params["tweet_mode"] = "extended";
    params["include_entities"] = "true";
    params["trim_user"] = "false";
    params["id"] = sanitizedStatus;
    params["include_ext_alt_text"] = "true";
    genericRequest(API_STATUSES_SHOW, STANDARD_REQ(TwitterApi::showStatus), true, params, true,
                   &TwitterApi::genericHandlerFinished, &TwitterApi::handleShowStatusError, useSecretIdentity);
}

void TwitterApi::showUser(const QString &screenName)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["include_entities"] = "true";
    params["screen_name"] = screenName;
    genericRequest(API_USERS_SHOW, STANDARD_REQ(TwitterApi::showUser), true, params, true);
}

void TwitterApi::showUserById(const QString &userId)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["include_entities"] = "true";
    params["user_id"] = userId;
    genericRequest(API_USERS_SHOW, "TwitterApi::showUserById", &TwitterApi::showUserSuccessful,
                   &TwitterApi::showUserError, true, params, true);
}

void TwitterApi::userTimeline(const QString &screenName, const bool &useSecretIdentity)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["count"] = "200";
    params["include_rts"] = "true";
    params["exclude_replies"] = "false";
    params["screen_name"] = screenName;
    params["include_ext_alt_text"] = "true";
    genericRequest(API_STATUSES_USER_TIMELINE, STANDARD_REQ(TwitterApi::userTimeline), true, params, true,
                   &TwitterApi::genericHandlerFinished, &TwitterApi::handleUserTimelineError,
                   useSecretIdentity);
}

void TwitterApi::followers(const QString &screenName)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["screen_name"] = screenName;
    params["count"] = "200";
    params["skip_status"] = "true";
    params["include_user_entities"] = "true";
    genericRequest(API_FOLLOWERS_LIST, STANDARD_REQ(TwitterApi::followers), true, params, true);
}

void TwitterApi::friends(const QString &screenName, const QString &cursor)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["screen_name"] = screenName;
    params["count"] = "200";
    params["skip_status"] = "true";
    params["include_user_entities"] = "true";
    genericRequest(API_FRIENDS_LIST, STANDARD_REQ(TwitterApi::friends), true, params, true);
}

void TwitterApi::followUser(const QString &screenName)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["screen_name"] = screenName;
    genericRequest(API_FRIENDSHIPS_CREATE, STANDARD_REQ(TwitterApi::followUser), false, params, false,
                   &TwitterApi::handleFollowUserFinished, &TwitterApi::genericHandlerFailure);
}

void TwitterApi::unfollowUser(const QString &screenName)
{
    ParametersList params;
    params["tweet_mode"] = "extended";
    params["screen_name"] = screenName;
    genericRequest(API_FRIENDSHIPS_DESTROY, STANDARD_REQ(TwitterApi::unfollowUser), false, params, true);
}

void TwitterApi::searchTweets(const QString &query)
{
    if (query.isEmpty()) {
        emit searchTweetsSuccessful(QVariantList());
        return;
    }

    QString searchString = QString(QUrl::toPercentEncoding(query));

    qDebug() << "TwitterApi::searchTweets" << searchString;
    QUrl url = QUrl(API_SEARCH_TWEETS);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("tweet_mode", "extended");
    urlQuery.addQueryItem("q", searchString);
    urlQuery.addQueryItem("count", "100");
    urlQuery.addQueryItem("include_entities", "true");
    urlQuery.addQueryItem("include_ext_alt_text", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("q"), query.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("100")));
    requestParameters.append(O0RequestParameter(QByteArray("include_entities"), QByteArray("true")));
    requestParameters.append(O0RequestParameter(QByteArray("include_ext_alt_text"), QByteArray("true")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleSearchTweetsError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleSearchTweetsFinished()));
}

void TwitterApi::searchUsers(const QString &query)
{
    if (query.isEmpty()) {
        emit searchUsersSuccessful(QVariantList());
        return;
    }

    QString searchString = QString(QUrl::toPercentEncoding(query));

    qDebug() << "TwitterApi::searchUsers" << searchString;
    QUrl url = QUrl(API_SEARCH_USERS);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("tweet_mode", "extended");
    urlQuery.addQueryItem("q", searchString);
    urlQuery.addQueryItem("count", "20");
    urlQuery.addQueryItem("include_entities", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("q"), query.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("20")));
    requestParameters.append(O0RequestParameter(QByteArray("include_entities"), QByteArray("true")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleSearchUsersError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleSearchUsersFinished()));
}

void TwitterApi::searchGeo(const QString &latitude, const QString &longitude)
{
    qDebug() << "TwitterApi::searchGeo" << latitude << longitude;
    QUrl url = QUrl(API_GEO_SEARCH);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("lat", latitude);
    urlQuery.addQueryItem("long", longitude);
    urlQuery.addQueryItem("max_results", "1");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("lat"), latitude.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("long"), longitude.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("max_results"), QByteArray("1")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleSearchGeoError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleSearchGeoFinished()));
}

void TwitterApi::favorite(const QString &statusId)
{
    qDebug() << "TwitterApi::favorite" << statusId;
    QUrl url = QUrl(API_FAVORITES_CREATE);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("id"), statusId.toUtf8()));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleFavoriteError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleFavoriteFinished()));
}

void TwitterApi::unfavorite(const QString &statusId)
{
    qDebug() << "TwitterApi::unfavorite" << statusId;
    QUrl url = QUrl(API_FAVORITES_DESTROY);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("id"), statusId.toUtf8()));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleUnfavoriteError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleUnfavoriteFinished()));
}

void TwitterApi::favorites(const QString &screenName)
{
    qDebug() << "TwitterApi::favorites" << screenName;
    QUrl url = QUrl(API_FAVORITES_LIST);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("tweet_mode", "extended");
    urlQuery.addQueryItem("count", "200");
    urlQuery.addQueryItem("include_entities", "true");
    urlQuery.addQueryItem("screen_name", screenName);
    urlQuery.addQueryItem("include_ext_alt_text", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("200")));
    requestParameters.append(O0RequestParameter(QByteArray("include_entities"), QByteArray("true")));
    requestParameters.append(O0RequestParameter(QByteArray("screen_name"), screenName.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("include_ext_alt_text"), QByteArray("true")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleFavoritesError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleFavoritesFinished()));
}

void TwitterApi::retweet(const QString &statusId)
{
    qDebug() << "TwitterApi::retweet" << statusId;
    QUrl url = QUrl(QString(API_STATUSES_RETWEET).replace(":id", statusId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleRetweetError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleRetweetFinished()));
}

void TwitterApi::retweetsFor(const QString &statusId)
{
    qDebug() << "TwitterApi::retweetUsers" << statusId;
    QUrl url = QUrl(QString(API_STATUSES_RETWEETS_FOR).replace(":id", statusId));
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("tweet_mode", "extended");
    urlQuery.addQueryItem("count", "21");
    urlQuery.addQueryItem("trim_user", "false");

    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("21")));
    requestParameters.append(O0RequestParameter(QByteArray("trim_user"), QByteArray("false")));

    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleRetweetsForError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleRetweetsForFinished()));
}

void TwitterApi::unretweet(const QString &statusId)
{
    qDebug() << "TwitterApi::unretweet" << statusId;
    QUrl url = QUrl(QString(API_STATUSES_UNRETWEET).replace(":id", statusId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleUnretweetError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleUnretweetFinished()));
}

void TwitterApi::destroyTweet(const QString &statusId)
{
    qDebug() << "TwitterApi::destroy" << statusId;
    QUrl url = QUrl(QString(API_STATUSES_DESTROY).replace(":id", statusId));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleDestroyError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleDestroyFinished()));
}

void TwitterApi::uploadImage(const QString &fileName)
{
    qDebug() << "TwitterApi::uploadImage" << fileName;
    QUrl url = QUrl(QString(API_MEDIA_UPLOAD));
    QNetworkRequest request(url);

    QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart imagePart;
    imagePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/octet-stream"));
    imagePart.setHeader(QNetworkRequest::ContentDispositionHeader, QVariant("form-data; name=\"media\""));

    QFile *file = new QFile(fileName);
    file->open(QIODevice::ReadOnly);
    QByteArray rawImage = file->readAll();
    imagePart.setBody(rawImage);
    file->setParent(multiPart);

    multiPart->append(imagePart);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();

    QNetworkReply *reply = requestor->post(request, requestParameters, multiPart);
    multiPart->setParent(reply);
    reply->setObjectName(fileName);

    ImageResponseHandler *imageResponseHandler = new ImageResponseHandler(fileName, this);
    imageResponseHandler->setParent(reply);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), imageResponseHandler, SLOT(handleImageUploadError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), imageResponseHandler, SLOT(handleImageUploadFinished()));
    connect(reply, SIGNAL(uploadProgress(qint64,qint64)), imageResponseHandler, SLOT(handleImageUploadProgress(qint64,qint64)));
}

void TwitterApi::uploadImageDescription(const QString &mediaId, const QString &description)
{
    qDebug() << "TwitterApi::uploadImageDescription" << mediaId << description;
    QUrl url = QUrl(API_MEDIA_METADATA_CREATE);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_JSON);
    request.setRawHeader(QByteArray("charset"), QByteArray("UTF-8"));

    QJsonObject alternativeTextObject;
    alternativeTextObject.insert("text", description);
    QJsonObject metadataObject;
    metadataObject.insert("alt_text", alternativeTextObject);
    metadataObject.insert("media_id", mediaId);

    QJsonDocument requestDocument(metadataObject);
    QByteArray jsonAsByteArray = requestDocument.toJson();
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(jsonAsByteArray.size()));

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    QNetworkReply *reply = requestor->post(request, requestParameters, jsonAsByteArray);

    ImageMetadataResponseHandler *imageMetadataResponseHandler = new ImageMetadataResponseHandler(mediaId, this);
    imageMetadataResponseHandler->setParent(reply);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), imageMetadataResponseHandler, SLOT(handleImageMetadataUploadError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), imageMetadataResponseHandler, SLOT(handleImageMetadataUploadFinished()));

}

void TwitterApi::downloadFile(const QString &address, const QString &fileName)
{
    qDebug() << "TwitterApi::downloadFile" << address << fileName;
    QUrl url = QUrl(address);
    QNetworkRequest request(url);
    QNetworkReply *reply = manager->get(request);

    DownloadResponseHandler *downloadResponseHandler = new DownloadResponseHandler(fileName, this);
    downloadResponseHandler->setParent(reply);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), downloadResponseHandler, SLOT(handleDownloadError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), downloadResponseHandler, SLOT(handleDownloadFinished()));
    connect(reply, SIGNAL(downloadProgress(qint64,qint64)), downloadResponseHandler, SLOT(handleDownloadProgress(qint64,qint64)));
}

void TwitterApi::directMessagesList(const QString &cursor)
{
    qDebug() << "TwitterApi::directMessagesList" << cursor;
    QUrl url = QUrl(API_DIRECT_MESSAGES_LIST);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("count", "50");
    if (!cursor.isEmpty()) {
        urlQuery.addQueryItem("cursor", cursor);
    }
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("50")));
    if (!cursor.isEmpty()) {
        requestParameters.append(O0RequestParameter(QByteArray("cursor"), cursor.toUtf8()));
    }
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleDirectMessagesListError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleDirectMessagesListFinished()));
}

void TwitterApi::directMessagesNew(const QString &text, const QString &recipientId)
{
    qDebug() << "TwitterApi::directMessagesNew" << recipientId;
    QUrl url = QUrl(API_DIRECT_MESSAGES_NEW);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_JSON);

    QJsonObject messageTargetObject;
    messageTargetObject.insert("recipient_id", recipientId);
    QJsonObject messageDataObject;
    messageDataObject.insert("text", text);
    QJsonObject messageCreateObject;
    messageCreateObject.insert("target", messageTargetObject);
    messageCreateObject.insert("message_data", messageDataObject);

    QJsonObject eventObject;
    eventObject.insert("type", QString("message_create"));
    eventObject.insert("message_create", messageCreateObject);

    QJsonObject requestObject;
    requestObject.insert("event", eventObject);

    QJsonDocument requestDocument(requestObject);
    QByteArray jsonAsByteArray = requestDocument.toJson();
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(jsonAsByteArray.size()));

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    QNetworkReply *reply = requestor->post(request, requestParameters, jsonAsByteArray);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleDirectMessagesNewError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleDirectMessagesNewFinished()));
}

void TwitterApi::trends(const QString &placeId)
{
    qDebug() << "TwitterApi::trends" << placeId;
    QUrl url = QUrl(API_TRENDS_PLACE);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("id", placeId);
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("id"), placeId.toUtf8()));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleTrendsError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleTrendsFinished()));
}

void TwitterApi::placesForTrends(const QString &latitude, const QString &longitude)
{
    qDebug() << "TwitterApi::placesForTrends" << latitude << longitude;
    QUrl url = QUrl(API_TRENDS_CLOSEST);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("lat", latitude);
    urlQuery.addQueryItem("long", longitude);
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("lat"), latitude.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("long"), longitude.toUtf8()));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handlePlacesForTrendsError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handlePlacesForTrendsFinished()));
}

void TwitterApi::userLists()
{
    qDebug() << "TwitterApi::userLists";
    QUrl url = QUrl(API_LISTS_LIST);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("reverse", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("reverse"), QByteArray("true")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleUserListsError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleUserListsFinished()));
}

void TwitterApi::listsMemberships()
{
    qDebug() << "TwitterApi::listsMemberships";
    QUrl url = QUrl(API_LISTS_MEMBERSHIPS);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("count", "100");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("100")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleListsMembershipsError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleListsMembershipsFinished()));
}

void TwitterApi::listMembers(const QString &listId)
{
    qDebug() << "TwitterApi::listsMembers" << listId;
    QUrl url = QUrl(API_LISTS_MEMBERS);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("list_id", listId);
    urlQuery.addQueryItem("count", "200");
    urlQuery.addQueryItem("skip_status", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("list_id"), listId.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("200")));
    requestParameters.append(O0RequestParameter(QByteArray("skip_status"), QByteArray("true")));
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleListMembersError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleListMembersFinished()));
}

void TwitterApi::listTimeline(const QString &listId, const QString &maxId)
{
    qDebug() << "TwitterApi::listTimeline" << listId << maxId;
    QUrl url = QUrl(API_LISTS_STATUSES);
    QUrlQuery urlQuery = QUrlQuery();
    urlQuery.addQueryItem("tweet_mode", "extended");
    urlQuery.addQueryItem("list_id", listId);
    if (!maxId.isEmpty()) {
        urlQuery.addQueryItem("max_id", maxId);
    }
    urlQuery.addQueryItem("count", "200");
    urlQuery.addQueryItem("include_ext_alt_text", "true");
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("tweet_mode"), QByteArray("extended")));
    requestParameters.append(O0RequestParameter(QByteArray("list_id"), listId.toUtf8()));
    requestParameters.append(O0RequestParameter(QByteArray("count"), QByteArray("200")));
    requestParameters.append(O0RequestParameter(QByteArray("include_ext_alt_text"), QByteArray("true")));
    if (!maxId.isEmpty()) {
        requestParameters.append(O0RequestParameter(QByteArray("max_id"), maxId.toUtf8()));
    }
    QNetworkReply *reply = requestor->get(request, requestParameters);

    if (maxId.isEmpty()) {
        connect(reply, SIGNAL(finished()), this, SLOT(handleListTimelineFinished()));
    } else {
        connect(reply, SIGNAL(finished()), this, SLOT(handleListTimelineLoadMoreFinished()));
    }
    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleListTimelineError(QNetworkReply::NetworkError)));

}

void TwitterApi::savedSearches()
{
    qDebug() << "TwitterApi::savedSearches";
    QUrl url = QUrl(API_SAVED_SEARCHES_LIST);
    QUrlQuery urlQuery = QUrlQuery();
    url.setQuery(urlQuery);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    QNetworkReply *reply = requestor->get(request, requestParameters);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleSavedSearchesError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleSavedSearchesFinished()));
}

void TwitterApi::saveSearch(const QString &query)
{
    qDebug() << "TwitterApi::saveSearch" << query;
    QUrl url = QUrl(QString(API_SAVED_SEARCHES_CREATE));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    requestParameters.append(O0RequestParameter(QByteArray("query"), query.toUtf8()));
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleSaveSearchError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleSaveSearchFinished()));
}

void TwitterApi::destroySavedSearch(const QString &id)
{
    qDebug() << "TwitterApi::destroySavedSearch" << id;
    QUrl url = QUrl(QString(API_SAVED_SEARCHES_DESTROY).replace(":id", id));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);

    QList<O0RequestParameter> requestParameters = QList<O0RequestParameter>();
    QByteArray postData = O1::createQueryParameters(requestParameters);

    QNetworkReply *reply = requestor->post(request, requestParameters, postData);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleDestroySavedSearchError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleDestroySavedSearchFinished()));
}

void TwitterApi::getOpenGraph(const QString &address)
{
    qDebug() << "TwitterApi::getOpenGraph" << address;
    QUrl url = QUrl(address);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Wayland; SailfishOS) Piepmatz (Not Firefox/52.0)");
    request.setRawHeader(QByteArray("Accept"), QByteArray("text/html,application/xhtml+xml"));
    request.setRawHeader(QByteArray("Accept-Charset"), QByteArray("utf-8"));
    request.setRawHeader(QByteArray("Connection"), QByteArray("close"));
    request.setRawHeader(QByteArray("Cache-Control"), QByteArray("max-age=0"));
    QNetworkReply *reply = manager->get(request);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleGetOpenGraphError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleGetOpenGraphFinished()));
}

void TwitterApi::getSingleTweet(const QString &tweetId, const QString &address)
{
    qDebug() << "TwitterApi::getSingleTweet" << tweetId << address;
    QUrl url = QUrl(address);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 (Wayland; SailfishOS) Piepmatz (Not Firefox/52.0)");
    request.setRawHeader(QByteArray("Accept-Charset"), QByteArray("utf-8"));
    request.setRawHeader(QByteArray("Connection"), QByteArray("close"));
    request.setRawHeader(QByteArray("Cache-Control"), QByteArray("max-age=0"));
    QNetworkReply *reply = manager->get(request);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleGetSingleTweetError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleGetSingleTweetFinished()));
}

void TwitterApi::getIpInfo()
{
    qDebug() << "TwitterApi::getIpInfo";
    QUrl url = QUrl("https://ipinfo.io/json");
    QNetworkRequest request(url);
    QNetworkReply *reply = manager->get(request);

    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(handleGetIpInfoError(QNetworkReply::NetworkError)));
    connect(reply, SIGNAL(finished()), this, SLOT(handleGetIpInfoFinished()));
}

void TwitterApi::controlScreenSaver(const bool &enabled)
{
    qDebug() << "TwitterApi::controlScreenSaver";
    QDBusConnection dbusConnection = QDBusConnection::connectToBus(QDBusConnection::SystemBus, "system");
    QDBusInterface dbusInterface("com.nokia.mce", "/com/nokia/mce/request", "com.nokia.mce.request", dbusConnection);

    if (enabled) {
        qDebug() << "Enabling screensaver";
        dbusInterface.call("req_display_cancel_blanking_pause");
    } else {
        qDebug() << "Disabling screensaver";
        dbusInterface.call("req_display_blanking_pause");
    }

}

void TwitterApi::handleAdditionalInformation(const QString &additionalInformation)
{
    qDebug() << "TwitterApi::handleAdditionalInformation" << additionalInformation;
    // For now only used to open downloaded files...
    QStringList argumentsList;
    argumentsList.append(additionalInformation);
    bool successfullyStarted = QProcess::startDetached("xdg-open", argumentsList);
    if (successfullyStarted) {
        qDebug() << "Successfully opened file " << additionalInformation;
    } else {
        qDebug() << "Error opening file " << additionalInformation;
    }
}

QVariantMap TwitterApi::parseErrorResponse(const QString &errorText, const QByteArray &responseText)
{
    qDebug() << "TwitterApi::parseErrorResponse" << errorText << responseText;
    QVariantMap errorResponse;
    errorResponse.insert("message", errorText);
    QJsonDocument jsonDocument = QJsonDocument::fromJson(responseText);
    if (jsonDocument.isObject()) {
        QJsonValue errorsValue = jsonDocument.object().value("errors");
        if (errorsValue.isArray()) {
            foreach (const QJsonValue &errorsValue, errorsValue.toArray()) {
                QJsonObject errorElementObject = errorsValue.toObject();
                errorResponse.insert("code", QString::number(errorElementObject.value("code").toInt()));
                errorResponse.insert("message", errorElementObject.value("message").toString());
            }
        }
    }
    return errorResponse;
}

void TwitterApi::_handleHomeTimelineFinishedHelper(const QString& title, QNetworkReply *reply, bool incrementalUpdate)
{
    qDebug() << QString("finished %1").arg(incrementalUpdate ? "incremental" : "non-incremental") << title;
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) return;

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit homeTimelineSuccessful(responseArray.toVariantList(), incrementalUpdate);
    } else {
        emit homeTimelineError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleHomeTimelineFinished(const QString &title, QNetworkReply *reply, ApiResultList successSignal, ApiResultError errorSignal)
{
    Q_UNUSED(successSignal); Q_UNUSED(errorSignal);
    _handleHomeTimelineFinishedHelper(title, reply, false);
}

void TwitterApi::handleHomeTimelineLoadMoreFinished(const QString &title, QNetworkReply *reply, TwitterApi::ApiResultList successSignal, ApiResultError errorSignal)
{
    Q_UNUSED(successSignal); Q_UNUSED(errorSignal);
    _handleHomeTimelineFinishedHelper(title, reply, true);
}

void TwitterApi::handleUserTimelineError(const QString &title, QNetworkReply *reply, QNetworkReply::NetworkError errorCode, ApiResultError errorSignal)
{
    Q_UNUSED(title); Q_UNUSED(errorSignal);
    qWarning() << "TwitterApi::handleUserTimelineError:" << (int)errorCode << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    QUrlQuery urlQuery(reply->request().url());
    if (reply->request().hasRawHeader(HEADER_NO_RECURSION)) {
        qDebug() << "Probably a secret identity response...";
    } else {
        qDebug() << "Standard response...";
    }
    // We use the secret identity if it exists, if we were blocked and if the previous request wasn't already a secret request
    if (secretIdentityRequestor != nullptr && parsedErrorResponse.value("code") == "136" && !reply->request().hasRawHeader(HEADER_NO_RECURSION)) {
        qDebug() << "Using secret identity for user " << urlQuery.queryItemValue("screen_name");
        this->userTimeline(urlQuery.queryItemValue("screen_name"), true);
    } else {
        emit userTimelineError(parsedErrorResponse.value("message").toString());
    }
}

void TwitterApi::handleShowStatusError(const QString &title, QNetworkReply *reply, QNetworkReply::NetworkError errorCode, ApiResultError errorSignal)
{
    Q_UNUSED(title); Q_UNUSED(errorSignal);
    qWarning() << "TwitterApi::handleShowStatusError:" << (int)errorCode << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    qDebug() << "Tweet couldn't be loaded for URL " << reply->request().url().toString() << ", errors: " << parsedErrorResponse;
    // emit showStatusError(parsedErrorResponse.value("message").toString());
    QUrlQuery urlQuery(reply->request().url());
    if (reply->request().hasRawHeader(HEADER_NO_RECURSION)) {
        qDebug() << "Probably a secret identity response...";
    } else {
        qDebug() << "Standard response...";
    }
    // We use the secret identity if it exists, if we were blocked and if the previous request wasn't already a secret request
    if (secretIdentityRequestor != nullptr && parsedErrorResponse.value("code") == "136" && !reply->request().hasRawHeader(HEADER_NO_RECURSION)) {
        qDebug() << "Using secret identity for tweet " << urlQuery.queryItemValue("id");
        this->showStatus(urlQuery.queryItemValue("id"), true);
    } else {
        QVariantMap fakeTweet;
        fakeTweet.insert("fakeTweet", true);
        QVariantMap fakeUser;
        fakeUser.insert("name", "");
        fakeUser.insert("verified", false);
        fakeUser.insert("protected", false);
        fakeUser.insert("profile_image_url_https", "");
        fakeTweet.insert("user", fakeUser);
        fakeTweet.insert("source", "Piepmatz");
        fakeTweet.insert("retweeted", false);
        fakeTweet.insert("favorited", false);
        QVariantMap fakeEntities;
        QVariantList fakeHashtags;
        fakeEntities.insert("hashtags", fakeHashtags);
        QVariantList fakeSymbols;
        fakeEntities.insert("symbols", fakeSymbols);
        QVariantList fakeUrls;
        fakeEntities.insert("urls", fakeUrls);
        QVariantList fakeMentions;
        fakeEntities.insert("user_mentions", fakeMentions);
        fakeTweet.insert("entities", fakeEntities);
        fakeTweet.insert("created_at", "Sun Jan 05 13:05:00 +0000 2020");
        fakeTweet.insert("id_str", urlQuery.queryItemValue("id"));
        fakeTweet.insert("full_text", parsedErrorResponse.value("message").toString());
        emit showStatusSuccessful(fakeTweet);
    }
}

void TwitterApi::handleFollowUserFinished()
{
    qDebug() << "TwitterApi::handleFollowUserFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        // Sometimes, Twitter still says "following": true here - strange isn't it?
        responseObject.remove("following");
        responseObject.insert("following", QJsonValue(true));
        emit followUserSuccessful(responseObject.toVariantMap());
    } else {
        emit followUserError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleUnfollowUserFinished()
{
    qDebug() << "TwitterApi::handleUnfollowUserFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        // Sometimes, Twitter still says "following": false here - strange isn't it?
        responseObject.remove("following");
        responseObject.insert("following", QJsonValue(false));
        emit unfollowUserSuccessful(responseObject.toVariantMap());
    } else {
        emit unfollowUserError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleSearchTweetsError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleSearchTweetsError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit searchTweetsError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleSearchTweetsFinished()
{
    qDebug() << "TwitterApi::handleSearchTweetsFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        // We try to remove duplicate tweets which come in due to retweets
        QJsonArray originalResultsArray = responseObject.value("statuses").toArray();
        QList<QString> foundStatusIds;
        QJsonArray resultsArray;
        for (int i = 0; i < originalResultsArray.size(); i++) {
            QJsonObject currentObject = originalResultsArray.at(i).toObject();
            QString currentStatusId;
            if (currentObject.contains("retweeted_status")) {
                currentStatusId = currentObject.value("retweeted_status").toObject().value("id_str").toString();
            } else {
                currentStatusId = currentObject.value("id_str").toString();
            }
            if (!foundStatusIds.contains(currentStatusId)) {
                resultsArray.append(currentObject);
                foundStatusIds.append(currentStatusId);
            }
        }
        emit searchTweetsSuccessful(resultsArray.toVariantList());
    } else {
        emit searchTweetsError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleSearchUsersError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleSearchUsersError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit searchUsersError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleSearchUsersFinished()
{
    qDebug() << "TwitterApi::handleSearchUsersFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit searchUsersSuccessful(responseArray.toVariantList());
    } else {
        emit searchUsersError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleSearchGeoError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleSearchGeoError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit searchGeoError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleSearchGeoFinished()
{
    qDebug() << "TwitterApi::handleSearchGeoFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit searchGeoSuccessful(responseObject.toVariantMap());
    } else {
        emit searchGeoError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleFavoriteError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleFavoriteError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit favoriteError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleFavoriteFinished()
{
    qDebug() << "TwitterApi::handleFavoriteFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit favoriteSuccessful(responseObject.toVariantMap());
    } else {
        emit favoriteError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleUnfavoriteError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleUnfavoriteError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit unfavoriteError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleUnfavoriteFinished()
{
    qDebug() << "TwitterApi::handleUnfavoriteFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit unfavoriteSuccessful(responseObject.toVariantMap());
    } else {
        emit unfavoriteError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleFavoritesError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleFavoritesError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit favoritesError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleFavoritesFinished()
{
    qDebug() << "TwitterApi::handleFavoritesFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit favoritesSuccessful(responseArray.toVariantList());
    } else {
        emit favoritesError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleRetweetError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleRetweetError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit retweetError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleRetweetFinished()
{
    qDebug() << "TwitterApi::handleRetweetFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit retweetSuccessful(responseObject.toVariantMap());
    } else {
        emit retweetError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleRetweetsForError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QString requestPath = reply->request().url().path();
    QRegExp statusRegex("(\\d+)\\.json");
    QString statusId;
    if (statusRegex.indexIn(requestPath) != -1) {
        statusId = statusRegex.cap(1);
    }
    qWarning() << "TwitterApi::handleRetweetUsersError:" << (int)error << reply->errorString() << reply->readAll() << statusId;
    emit retweetsForError(statusId, reply->errorString());
}

void TwitterApi::handleRetweetsForFinished()
{
    qDebug() << "TwitterApi::handleRetweetsForFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }
    QString requestPath = reply->request().url().path();
    QRegExp statusRegex("(\\d+)\\.json");
    QString statusId;
    if (statusRegex.indexIn(requestPath) != -1) {
        statusId = statusRegex.cap(1);
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit retweetsForSuccessful(statusId, responseArray.toVariantList());
    } else {
        emit retweetsForError(statusId, DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleUnretweetError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleUnretweetError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit unretweetError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleUnretweetFinished()
{
    qDebug() << "TwitterApi::handleUnretweetFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit unretweetSuccessful(responseObject.toVariantMap());
    } else {
        emit unretweetError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleDestroyError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleDestroyError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit destroyError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleDestroyFinished()
{
    qDebug() << "TwitterApi::handleDestroyFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit destroySuccessful(responseObject.toVariantMap());
    } else {
        emit destroyError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleDirectMessagesListError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleDirectMessagesListError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit directMessagesListError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleDirectMessagesListFinished()
{
    qDebug() << "TwitterApi::handleDirectMessagesListFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit directMessagesListSuccessful(responseObject.toVariantMap());
    } else {
        emit directMessagesListError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleDirectMessagesNewError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleDirectMessagesNewError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit directMessagesNewError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleDirectMessagesNewFinished()
{
    qDebug() << "TwitterApi::handleDirectMessagesNewFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit directMessagesNewSuccessful(responseObject.toVariantMap());
    } else {
        emit directMessagesNewError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleTrendsError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleTrendsError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit trendsError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleTrendsFinished()
{
    qDebug() << "TwitterApi::handleTrendsFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit trendsSuccessful(responseArray.toVariantList());
    } else {
        emit trendsError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handlePlacesForTrendsError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handlePlacesForTrendsError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit placesForTrendsError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handlePlacesForTrendsFinished()
{
    qDebug() << "TwitterApi::handlePlacesForTrendsFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit placesForTrendsSuccessful(responseArray.toVariantList());
    } else {
        emit placesForTrendsError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleUserListsError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleUserListsError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit userListsError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleUserListsFinished()
{
    qDebug() << "TwitterApi::handleUserListsFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit userListsSuccessful(responseArray.toVariantList());
    } else {
        emit userListsError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleListsMembershipsError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleListsMembershipsError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit listsMembershipsError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleListsMembershipsFinished()
{
    qDebug() << "TwitterApi::handleListsMembershipsFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit listsMembershipsSuccessful(responseObject.toVariantMap());
    } else {
        emit listsMembershipsError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleListMembersError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleListsMembersError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit listMembersError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleListMembersFinished()
{
    qDebug() << "TwitterApi::handleListsMembersFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit listMembersSuccessful(responseObject.toVariantMap());
    } else {
        emit listMembersError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleListTimelineError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleListTimelineError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit listTimelineError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleListTimelineFinished()
{
    qDebug() << "TwitterApi::handleListTimelineFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit listTimelineSuccessful(responseArray.toVariantList(), false);
    } else {
        emit listTimelineError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleListTimelineLoadMoreFinished()
{
    qDebug() << "TwitterApi::handleListTimelineLoadMoreFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit listTimelineSuccessful(responseArray.toVariantList(), true);
    } else {
        emit listTimelineError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleSavedSearchesError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleSavedSearchesError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit savedSearchesError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleSavedSearchesFinished()
{
    qDebug() << "TwitterApi::handleSavedSearchesFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isArray()) {
        QJsonArray responseArray = jsonDocument.array();
        emit savedSearchesSuccessful(responseArray.toVariantList());
    } else {
        emit savedSearchesError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleSaveSearchError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleSaveSearchError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit saveSearchError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleSaveSearchFinished()
{
    qDebug() << "TwitterApi::handleSaveSearchFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit saveSearchSuccessful(responseObject.toVariantMap());
    } else {
        emit saveSearchError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleDestroySavedSearchError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleDestroySavedSearchError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit destroySavedSearchError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleDestroySavedSearchFinished()
{
    qDebug() << "TwitterApi::handleDestroySavedSearchFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit destroySavedSearchSuccessful(responseObject.toVariantMap());
    } else {
        emit destroySavedSearchError(DEFAULT_ERROR_MESSAGE);
    }
}

void TwitterApi::handleGetOpenGraphError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleGetOpenGraphFinished:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit getOpenGraphError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleGetOpenGraphFinished()
{
    qDebug() << "TwitterApi::handleGetOpenGraphFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QString requestAddress = reply->request().url().toString();

    QVariant contentTypeHeader = reply->header(QNetworkRequest::ContentTypeHeader);
    if (!contentTypeHeader.isValid()) {
        return;
    }
    qDebug() << "Open Graph content type header: " << contentTypeHeader.toString();
    if (contentTypeHeader.toString().indexOf("text/html", 0, Qt::CaseInsensitive) == -1) {
        qDebug() << requestAddress + " is not HTML, not checking Open Graph data...";
        return;
    }

    QString charset = "UTF-8";
    QRegularExpression charsetRegularExpression("charset\\s*\\=[\\s\\\"\\\']*([^\\s\\\"\\\'\\,>]*)");
    QRegularExpressionMatchIterator matchIterator = charsetRegularExpression.globalMatch(contentTypeHeader.toString());
    QStringList availableCharsets;
    while (matchIterator.hasNext()) {
        QRegularExpressionMatch nextMatch = matchIterator.next();
        QString currentCharset = nextMatch.captured(1).toUpper();
        qDebug() << "Available Open Graph charset: " << currentCharset;
        availableCharsets.append(currentCharset);
    }
    if (availableCharsets.size() > 0 && !availableCharsets.contains("UTF-8")) {
        // If we haven't received the requested UTF-8, we simply use the last one which we received in the header
        charset = availableCharsets.last();
    }
    qDebug() << "Open Graph Charset for " << requestAddress << ": " << charset;

    QByteArray rawDocument = reply->readAll();
    QTextCodec *codec = QTextCodec::codecForName(charset.toUtf8());
    QString resultDocument = codec->toUnicode(rawDocument);

    QVariantMap openGraphData;
    QRegExp urlRegex("\\<meta\\s+property\\=\\\"og\\:url\\\"\\s+content\\=\\\"([^\\\"]+)\\\"");
    if (urlRegex.indexIn(resultDocument) != -1) {
        openGraphData.insert("url", urlRegex.cap(1));
    }
    QRegExp imageRegex("\\<meta\\s+property\\=\\\"og\\:image\\\"\\s+content\\=\\\"([^\\\"]+)\\\"");
    if (imageRegex.indexIn(resultDocument) != -1) {
        openGraphData.insert("image", imageRegex.cap(1));
    }
    QRegExp descriptionRegex("\\<meta\\s+property\\=\\\"og\\:description\\\"\\s+content\\=\\\"([^\\\"]+)\\\"");
    if (descriptionRegex.indexIn(resultDocument) != -1) {
        openGraphData.insert("description", descriptionRegex.cap(1));
    }
    QRegExp titleRegex("\\<meta\\s+property\\=\\\"og\\:title\\\"\\s+content\\=\\\"([^\\\"]+)\\\"");
    if (titleRegex.indexIn(resultDocument) != -1) {
        openGraphData.insert("title", titleRegex.cap(1));
    }

    if (openGraphData.isEmpty()) {
        emit getOpenGraphError(requestAddress + " does not contain Open Graph data");
    } else {
        // Always using request URL to be able to compare results
        openGraphData.insert("url", requestAddress);
        if (!openGraphData.contains("title")) {
            openGraphData.insert("title", openGraphData.value("url"));
        }
        qDebug() << "Open Graph data found for " + requestAddress;
        emit getOpenGraphSuccessful(openGraphData);
    }
}

void TwitterApi::handleGetSingleTweetError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleGetSingleTweetError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
}

void TwitterApi::handleGetSingleTweetFinished()
{
    qDebug() << "TwitterApi::handleGetSingleTweetFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QString requestAddress = reply->request().url().toString();

    QVariant contentTypeHeader = reply->header(QNetworkRequest::ContentTypeHeader);
    if (!contentTypeHeader.isValid()) {
        qDebug() << "Content Type response header is invalid, unable to check for conversation!";
        return;
    }
    if (contentTypeHeader.toString().indexOf("text/html", 0, Qt::CaseInsensitive) == -1) {
        qDebug() << requestAddress + " is not HTML, not checking tweet result data...";
        return;
    }

    QRegExp tweetIdRegex("status\\/(\\d+)");
    QString currentTweetId;
    if (tweetIdRegex.indexIn(requestAddress) != -1) {
        currentTweetId = tweetIdRegex.cap(1);
    }

    QString resultDocument(reply->readAll());
    QGumboDocument parsedResult = QGumboDocument::parse(resultDocument);
    QGumboNode root = parsedResult.rootNode();

    // === DEBUG ===
    // ContentExtractor contentExtractor(this, &root);
    // contentExtractor.parse();
    // === DEBUG ===


    QGumboNodes tweetNodes = root.getElementsByClassName("tweet");
    QVariantList relatedTweets;
    for (QGumboNode &tweetNode : tweetNodes) {
        QStringList tweetClassList = tweetNode.classList();
        if (!tweetClassList.contains("promoted-tweet")) {
            QString otherTweetId = tweetNode.getAttribute("data-tweet-id");
            if (!otherTweetId.isEmpty()) {
                qDebug() << "Found Tweet ID: " << otherTweetId;
                relatedTweets.append(otherTweetId);
            }
        }
    }

    if (!relatedTweets.isEmpty()) {
        qDebug() << "Found other tweets, let's build a conversation!";
        TweetConversationHandler *conversationHandler = new TweetConversationHandler(this, currentTweetId, relatedTweets, this);
        connect(conversationHandler, SIGNAL(tweetConversationCompleted(QString, QVariantList)), this, SLOT(handleTweetConversationReceived(QString, QVariantList)));
        conversationHandler->buildConversation();
    }

}

void TwitterApi::handleTweetConversationReceived(QString tweetId, QVariantList receivedTweets)
{
    emit tweetConversationReceived(tweetId, receivedTweets);
}

void TwitterApi::handleGetIpInfoError(QNetworkReply::NetworkError error)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    qWarning() << "TwitterApi::handleGetIpInfoError:" << (int)error << reply->errorString();
    QVariantMap parsedErrorResponse = parseErrorResponse(reply->errorString(), reply->readAll());
    emit getIpInfoError(parsedErrorResponse.value("message").toString());
}

void TwitterApi::handleGetIpInfoFinished()
{
    qDebug() << "TwitterApi::handleGetIpInfoFinished";
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QJsonDocument jsonDocument = QJsonDocument::fromJson(reply->readAll());
    if (jsonDocument.isObject()) {
        QJsonObject responseObject = jsonDocument.object();
        emit getIpInfoSuccessful(responseObject.toVariantMap());
    } else {
        emit getIpInfoError(DEFAULT_ERROR_MESSAGE);
    }
}
