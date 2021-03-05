#include "PrecompiledHeader.h"
#include <combaseapi.h> // GUID stuff
#include "LockedResource.h"
#include <mutex>
SessionMgr::SessionMgr()
{
}

SessionMgr::~SessionMgr()
{
}

void SessionMgr::StartServer()
{
    this->server.config.port = 7011;
    auto& echo = this->server.endpoint["^/MonacoServer/?$"];

    echo.on_message = 
        [this](std::shared_ptr<WsServer::Connection> connection, std::shared_ptr<WsServer::Message> message) 
        { this->OnServer_Message(connection, message); };

    echo.on_open = 
        [this](std::shared_ptr<WsServer::Connection> connection) 
        { this->OnServer_Open(connection); };

    // See RFC 6455 7.4.1. for status codes
    echo.on_close = 
        [this](std::shared_ptr<WsServer::Connection> connection, int status, const std::string& reason) 
        { this->OnServer_Close(connection, status, reason); };

    echo.on_error = 
        [this](std::shared_ptr<WsServer::Connection> connection, const SimpleWeb::error_code& ec) 
        { this->OnServer_Error(connection, ec); };

    std::cout << "Setting up server\n";
    this->serverThread = std::thread(
        [this]() 
        {
            std::cout << "Setting up server\n";
            // Start WS-server
            this->server.start();
        });

    //server_thread.join();
}

void SessionMgr::OnServer_Message(std::shared_ptr<WsServer::Connection> connection, std::shared_ptr<WsServer::Message> message)
{
    std::string msg = message->string();

    json::jobject result = json::jobject::parse(msg);
    std::string msgTy = result["msg"].as_string();

    std::cout << "Server: Message received: " << msg << std::endl;
    if (msgTy == "respauth")
    {
        std::string authKey = result["data"].as_object()["key"].as_string();
        std::string userName = result["data"].as_object()["name"].as_string();
        std::string room = result["data"].as_object()["room"].as_string();

        // TODO: Validate room name, including empty room name

        bool wasInAuthState = false;

        { LOCK_SCOPE(lrAwaitVerifs, awaitVerifs)
            auto findIt = awaitVerifs.find(connection);
            if (findIt != awaitVerifs.end())
            {
                awaitVerifs.erase(findIt);
                wasInAuthState = true;
            }
        }

        if (wasInAuthState == true)
        {
            UserConPtr newUserPtr = UserConPtr(new UserCon(userName, connection));

            {
                LOCK_SCOPE(lrSessions, sessions)
                LOCK_SCOPE(lrUserLookup, userLookup)

                auto itSess = sessions.find(room);
                if (itSess == sessions.end())
                {
                    std::cout << "Creating new room " << room << std::endl;
                    SessionPtr newSession = SessionPtr(new Session(this, room));
                    sessions[room] = newSession;

                    std::cout << "Added user " << userName << " to new room " << room << std::endl;
                    newUserPtr->session = newSession;
                    newSession->StageNewUser(newUserPtr);
                    newSession->StartSession();
                    userLookup[connection] = newUserPtr;
                }
                else
                {
                    std::cout << "Added user " << userName << " to existing room " << room << std::endl;
                    newUserPtr->session = itSess->second;
                    itSess->second->StageNewUser(newUserPtr);
                    userLookup[connection] = newUserPtr;
                }
            
            }
        }
        else
        {
            auto send_stream = std::make_shared<WsServer::SendStream>();
            *send_stream << std::string(R"({"msg":"dc", "data":"Attempting to log in without proper handshake."})");
            connection->send(send_stream);
            connection->send_close(0);
        }
    }
    else if (msgTy == "chat")
    {
        // The session manager is (currently) in charge of intercepting all web server
        // messages, it we can't handle chat messages directly. Instead we'll intercept
        // them and delegate them to the proper session for them to do any error checking
        // and broadcasting.
        { LOCK_SCOPE(lrUserLookup, userLookup)
            auto itFind = userLookup.find(connection);

            if (itFind != userLookup.end())
            {
                SessionPtr session = itFind->second->session;
                { LOCK_SCOPE3(session->lrChatMessages, ChatMessages, chatMessages)
                    MsgChat msgChat;
                    msgChat.user = itFind->second;
                    msgChat.message = result["data"].as_string();
                    chatMessages.push_back(msgChat);
                }
            }
        }
    }
    else if (msgTy == "addmarker")
    {
    } // Unknown if this will be implemented, just brainstorming.
    else if (msgTy == "rmmarker")
    {
    } // Unknown if this will be implemented, just brainstorming.
    else if (msgTy == "addgeom")
    {
    } // Unknown if this will be implemented, just brainstorming.
    else if (msgTy == "rmgeom")
    {
    } // Unknown if this will be implemented, just brainstorming.

   //auto send_stream = std::make_shared<WsServer::SendStream>();
   //*send_stream << message_str;
   //// connection->send is an asynchronous function
   //connection->send(
   //    send_stream, 
   //    [](const SimpleWeb::error_code& ec) 
   //    {
   //        if (ec) {
   //            std::cout << "Server: Error sending message. " <<
   //            // See http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio/reference.html, Error Codes for error code meanings
   //            "Error: " << ec << ", error message: " << ec.message() << std::endl;
   //    }
   //});
}

void SessionMgr::OnServer_Open(std::shared_ptr<WsServer::Connection> connection)
{
    GUID guid;
    CoCreateGuid(&guid);

    // https://stackoverflow.com/a/27621890/2680066
    char guidStr[37];
    sprintf_s(
        guidStr,
        "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);

    std::string guidString(guidStr);

    { LOCK_SCOPE(lrAwaitVerifs, awaitVerifs)
        awaitVerifs[connection] = guidStr;
    }

    std::stringstream ss;
    ss << "User opened connection : Sending authorization query with unique key " << guidStr << "." << std::endl;

    std::cout << ss.str();

    auto send_stream = std::make_shared<WsServer::SendStream>();
    *send_stream << std::string(R"({"msg":"auth", "data":")") + guidString + R"("})";
    connection->send(send_stream);
}

void SessionMgr::OnServer_Close(std::shared_ptr<WsServer::Connection> connection, int status, const std::string& reason)
{
    std::cout << "Server: Closed connection " << connection.get() << " with status code " << status << std::endl;

    { LOCK_SCOPE(lrAwaitVerifs, awaitVerifs)
        auto findIt = awaitVerifs.find(connection);
        if (findIt != awaitVerifs.end())
        {
            awaitVerifs.erase(findIt);
        }
    }

    { LOCK_SCOPE(lrUserLookup, userLookup)
        auto itFindUCP = userLookup.find(connection);
        if (itFindUCP != userLookup.end())
        {
            UserConPtr user = itFindUCP->second;
            { LOCK_SCOPE3(user->session->lrUsersExit, usersExit, usersExit)
                usersExit.push_back(user);
            }
            std::cout << "Closed user was queued for removal from session.";

            userLookup.erase(itFindUCP);
            std::cout << "Closed user was removed from global server directory.";

        }
    }
}

void SessionMgr::OnServer_Error(std::shared_ptr<WsServer::Connection> connection, const SimpleWeb::error_code& ec)
{
    std::cout << "Server: Error in connection " << connection.get() << ". " << "Error: " << ec << ", error message: " << ec.message() << std::endl;

    {LOCK_SCOPE(lrAwaitVerifs, awaitVerifs)
        auto findRem = awaitVerifs.find(connection);
        if (findRem != awaitVerifs.end())
            awaitVerifs.erase(findRem);
    }
}
