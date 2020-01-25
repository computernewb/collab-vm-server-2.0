#pragma once
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/functional/hash.hpp>
#include <filesystem>
#include <gsl/span>
#include <memory>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <capnp/blob.h>
#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>
#include <stdio.h>

#include "capnp-list.hpp"
#include "CaseInsensitiveUtils.hpp"
#include "CollabVm.capnp.h"
#include "CollabVmCommon.hpp"
#include "CollabVmChatRoom.hpp"
#include "CollabVmGuacamoleClient.hpp"
#include "SocketMessage.hpp"
#include "Database/Database.h"
#include "GuacamoleClient.hpp"
#include "GuacamoleScreenshot.hpp"
#include "CaptchaVerifier.hpp"
#include "StrandGuard.hpp"
#include "Totp.hpp"
#include "TurnController.hpp"
#include "UserChannel.hpp"
#include "AdminVirtualMachine.hpp"
#include "IPData.hpp"

namespace CollabVm::Server
{
  template <typename TServer>
  class CollabVmServer final : public TServer
  {
    template <typename T>
    using StrandGuard = StrandGuard<boost::asio::io_context::strand, T>;
    using SessionId = Database::SessionId;

  public:
    constexpr static auto global_channel_id = 0;

    template <typename TSocket>
    class CollabVmSocket final
      : public TSocket,
        public TurnController<
          std::shared_ptr<CollabVmSocket<TSocket>>>::UserTurnData
    {
      using SessionMap = std::unordered_map<
        SessionId,
        std::shared_ptr<CollabVmSocket>
        >;

    public:
      struct UserData
      {
        std::string username;
        CollabVmServerMessage::UserType user_type;
        typename TSocket::IpAddress::IpBytes ip_address;
        UserVoteData vote_data;

        bool IsAdmin() const {
          return user_type == CollabVmServerMessage::UserType::ADMIN;
        }
      };

      CollabVmSocket(boost::asio::io_context& io_context,
                     const std::filesystem::path& doc_root,
                     CollabVmServer& server)
        : TSocket(io_context, doc_root),
          server_(server),
          send_queue_(io_context),
          chat_rooms_(io_context),
          username_(io_context)
      {
      }

      ~CollabVmSocket() noexcept override { }

      class CollabVmMessageBuffer : public TSocket::MessageBuffer
      {
        capnp::FlatArrayMessageReader reader;
      public:
        CollabVmMessageBuffer() : reader(nullptr) {}
	~CollabVmMessageBuffer() noexcept override { }
        virtual capnp::FlatArrayMessageReader& CreateReader() = 0;

        template<typename TBuffer>
        capnp::FlatArrayMessageReader& CreateReader(TBuffer& buffer) {
          const auto buffer_data = buffer.data();
          const auto array_ptr = kj::ArrayPtr<const capnp::word>(
            static_cast<const capnp::word*>(buffer_data.data()),
            buffer_data.size() / sizeof(capnp::word));
          // TODO: Considering using capnp::ReaderOptions with lower limits
          reader = capnp::FlatArrayMessageReader(array_ptr);
          return reader;
        }
      };

      class CollabVmStaticMessageBuffer final : public CollabVmMessageBuffer
      {
        boost::beast::flat_static_buffer<1024> buffer;
      public:
	~CollabVmStaticMessageBuffer() noexcept override { }
        void StartRead(std::shared_ptr<TSocket>&& socket) override
        {
          socket->ReadWebSocketMessage(std::move(socket),
                                       std::static_pointer_cast<
                                         CollabVmStaticMessageBuffer>(
                                         CollabVmMessageBuffer::
                                         shared_from_this()));
        }
        auto& GetBuffer()
        {
          return buffer;
        }
        capnp::FlatArrayMessageReader& CreateReader() override
        {
          return CollabVmMessageBuffer::CreateReader(buffer);
        }
      };

      class CollabVmDynamicMessageBuffer final : public CollabVmMessageBuffer
      {
        boost::beast::flat_buffer buffer;
      public:
	~CollabVmDynamicMessageBuffer() noexcept override = default;

        void StartRead(std::shared_ptr<TSocket>&& socket) override
        {
          socket->ReadWebSocketMessage(std::move(socket),
                                       std::static_pointer_cast<
                                         CollabVmDynamicMessageBuffer>(
                                         CollabVmMessageBuffer::
                                         shared_from_this()));
        }
        auto& GetBuffer()
        {
          return buffer;
        }
        capnp::FlatArrayMessageReader& CreateReader() override
        {
          return CollabVmMessageBuffer::CreateReader(buffer);
        }
      };

      std::shared_ptr<typename TSocket::MessageBuffer> CreateMessageBuffer() override {
        return is_admin_
                 ? std::static_pointer_cast<typename TSocket::MessageBuffer>(
                   std::make_shared<CollabVmDynamicMessageBuffer>())
                 : std::static_pointer_cast<typename TSocket::MessageBuffer>(
                   std::make_shared<CollabVmStaticMessageBuffer>());
      }

      void OnPreConnect() override
      {
        server_.GetIPData(TSocket::GetIpAddress(),
          [this, self = shared_from_this()](auto& ip_data) {
            ip_data_ = ip_data;
            server_.settings_.dispatch([this, self = std::move(self)](auto& settings) {
              const auto max_connections_enabled =
                settings.GetServerSetting(ServerSetting::Setting::MAX_CONNECTIONS_ENABLED)
                .getMaxConnectionsEnabled();
              const auto max_connections =
                settings.GetServerSetting(ServerSetting::Setting::MAX_CONNECTIONS)
                .getMaxConnections();
              ip_data_->dispatch(
                [this, self = std::move(self), max_connections_enabled, max_connections]
                (auto& ip_data) {
                  if (max_connections_enabled
                      && ++ip_data.connections > max_connections) {
                    TSocket::Close();
                    return;
                  }
                  TSocket::OnPreConnect();
              });
            });
          });
      }

      void OnConnect() override
      {
        server_.settings_.dispatch([this, self = shared_from_this()](auto& settings) {
          is_captcha_required_ =
            settings.GetServerSetting(ServerSetting::Setting::CAPTCHA_REQUIRED)
                    .getCaptchaRequired();
        });
      }

      void OnMessage(
        std::shared_ptr<typename TSocket::MessageBuffer>&& buffer) override
      {
        try
        {
          HandleMessage(std::move(std::static_pointer_cast<CollabVmMessageBuffer>(buffer)));
        }
        catch (...)
        {
          TSocket::Close();
        }
      }

      void HandleMessage(std::shared_ptr<CollabVmMessageBuffer>&& buffer)
      {
        auto& reader = buffer->CreateReader();
        auto message = reader.template getRoot<CollabVmClientMessage>().getMessage();

        switch (message.which())
        {
        case CollabVmClientMessage::Message::CONNECT_TO_CHANNEL:
        {
          const auto channel_id = message.getConnectToChannel();
          username_.dispatch([
            this, self = shared_from_this(), channel_id]
            (auto& username) {
            auto connect_to_channel = [this, self = shared_from_this(), channel_id](auto& username) {
              auto connect_to_channel =
                [this, self = shared_from_this(), username]
              (auto& channel) mutable
              {
                LeaveVmList();
                if (connected_vm_id_)
                {
                  server_.virtual_machines_.dispatch(
                    [channel_id = connected_vm_id_, self = shared_from_this()]
                    (auto& virtual_machines) mutable
                    {
                      const auto virtual_machine = virtual_machines.
                        GetAdminVirtualMachine(channel_id);
                      if (!virtual_machine) {
                        return;
                      }
                      virtual_machine->GetUserChannel([self = std::move(self)]
                        (auto& channel) {
                          channel.RemoveUser(std::move(self));
                        });
                    });
                }
                connected_vm_id_ = channel.GetId();
                auto socket_message = SocketMessage::CreateShared();
                auto& message_builder = socket_message->GetMessageBuilder();
                auto connect_result =
                  message_builder.initRoot<CollabVmServerMessage>()
                  .initMessage()
                  .initConnectResponse()
                  .initResult();
                auto connectSuccess = connect_result.initSuccess();
                channel.GetChatRoom().GetChatHistory(connectSuccess);
                connectSuccess.setUsername(username);
                connectSuccess.setCaptchaRequired(is_captcha_required_);
                QueueMessage(std::move(socket_message));
                auto user_data = UserData();
                user_data.username = username;
                user_data.user_type = GetUserType();
                user_data.ip_address = TSocket::GetIpAddress().AsBytes();
                channel.AddUser(user_data, std::move(self));
              };
              if (channel_id == global_channel_id)
              {
                if (is_in_global_chat_)
                {
                  return;
                }
                is_in_global_chat_ = true;
                server_.global_chat_room_.dispatch(std::move(connect_to_channel));
              }
              else
              {
                server_.virtual_machines_.dispatch(
                  [this, channel_id, connect_to_channel = std::move(connect_to_channel)]
                  (auto& virtual_machines) mutable
                  { 
                    const auto virtual_machine = virtual_machines.
                      GetAdminVirtualMachine(channel_id);
                    if (!virtual_machine)
                    {
                      auto socket_message = SocketMessage::CreateShared();
                      auto& message_builder = socket_message->GetMessageBuilder();
                      auto connect_result =
                        message_builder.initRoot<CollabVmServerMessage>()
                        .initMessage()
                        .initConnectResponse()
                        .initResult();
                      connect_result.setFail();
                      QueueMessage(std::move(socket_message));
                      return;
                    }
                    virtual_machine->GetSettings(
                      [this, virtual_machine, connect_to_channel=std::move(connect_to_channel)]
                      (auto& settings) {
                        if (settings.GetSetting(VmSetting::Setting::DISALLOW_GUESTS)
                                    .getDisallowGuests()
                            && !is_logged_in_)
                        {
                          auto socket_message = SocketMessage::CreateShared();
                          auto& message_builder = socket_message->GetMessageBuilder();
                          auto connect_result =
                            message_builder.initRoot<CollabVmServerMessage>()
                            .initMessage()
                            .initConnectResponse()
                            .initResult();
                          connect_result.setFail();
                          QueueMessage(std::move(socket_message));
                          return;
                        }

                        virtual_machine->GetUserChannel(std::move(connect_to_channel));
                      });
                  });
              }
            };
            if (username.empty())
            {
              GenerateUsername(std::move(connect_to_channel));
            }
            else
            {
              connect_to_channel(username);
            }
          });
          break;
        }
        case CollabVmClientMessage::Message::CAPTCHA_COMPLETED:
        {
          server_.captcha_verifier_.Verify(
            message.getCaptchaCompleted().cStr(),
            [this, self = shared_from_this(),
              buffer = std::move(buffer)](bool is_valid)
          {
            is_captcha_required_ = !is_valid;
          });
          break;
        }
        case CollabVmClientMessage::Message::TURN_REQUEST:
        {
          if (!connected_vm_id_ || is_captcha_required_)
          {
            break;
          }
          server_.virtual_machines_.dispatch([
            this, self = shared_from_this()]
            (auto& virtual_machines) mutable
            {
              const auto virtual_machine = virtual_machines.
                GetAdminVirtualMachine(connected_vm_id_);
              if (!virtual_machine)
              {
                return;
              }
              virtual_machine->RequestTurn(std::move(self));
            });
          break;
        }
        case CollabVmClientMessage::Message::VOTE:
        {
          if (!connected_vm_id_ || is_captcha_required_)
          {
            break;
          }
          const auto voted_yes = message.getVote();
          server_.virtual_machines_.dispatch([
            this, self = shared_from_this(), voted_yes]
            (auto& virtual_machines) mutable
            {
              const auto virtual_machine = virtual_machines.
                GetAdminVirtualMachine(connected_vm_id_);
              if (!virtual_machine)
              {
                return;
              }
              virtual_machine->Vote(std::move(self), voted_yes);
            });
          break;
        }
        case CollabVmClientMessage::Message::GUAC_INSTR:
        {
          if (!connected_vm_id_ || is_captcha_required_)
          {
            break;
          }
          server_.virtual_machines_.dispatch([
            this, self = shared_from_this(),
            channel_id = connected_vm_id_, message, buffer = std::move(buffer)]
            (auto& virtual_machines) mutable
            {
              const auto virtual_machine = virtual_machines.
                GetAdminVirtualMachine(channel_id);
              if (!virtual_machine)
              {
                return;
              }
              virtual_machine->ReadInstruction(
                std::move(self),
                [this, self, buffer = std::move(buffer), message]() {
                  return message.getGuacInstr();
                });
            });
          break;
        }
        case CollabVmClientMessage::Message::CHANGE_USERNAME:
          {
            if (is_captcha_required_)
            {
              break;
            }
            if (is_logged_in_)
            {
              // Registered users can't change their usernames
              break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_username_change_ < Common::username_change_rate_limit)
            {
              break;
            }
            const auto new_username = message.getChangeUsername();
            if (!CollabVm::Common::ValidateUsername({
              new_username.begin(), new_username.size()
            }))
            {
              break;
            }
            username_.dispatch(
              [this, self = shared_from_this(), buffer = std::move(buffer), new_username]
              (auto& username) {
                if (username == std::string_view(new_username.cStr(), new_username.size())) {
                  return;
                }
                server_.guests_.dispatch(
                  [this, self = shared_from_this(),
                  buffer = std::move(buffer), new_username, current_username = username](auto& guests)
                {
                  const auto is_username_taken =
                    !std::get<bool>(guests.insert({ new_username, shared_from_this() }));
                  if (is_username_taken)
                  {
                    auto socket_message = SocketMessage::CreateShared();
                    auto message = socket_message->GetMessageBuilder()
                      .initRoot<CollabVmServerMessage>()
                      .initMessage();
                    message.setUsernameTaken();
                    QueueMessage(std::move(socket_message));
                    return;
                  }
                  guests.erase(current_username);
                  SetUserData(std::string(new_username));
                });
              });
            break;
          }
        case CollabVmClientMessage::Message::CHANGE_PASSWORD_REQUEST:
          if (!is_logged_in_)
          {
            break;
          }
          username_.dispatch(
            [this, self = shared_from_this(),
            buffer = std::move(buffer), message]
            (auto& username) {
              auto lambda = [this, self = shared_from_this(),
                buffer = std::move(buffer), message, username]()
              {
                const auto change_password_request =
                  message.getChangePasswordRequest();
                const auto success = server_.db_.ChangePassword(
                  username,
                  change_password_request.getOldPassword(),
                  change_password_request.getNewPassword());
                auto socket_message = SocketMessage::CreateShared();
                socket_message->GetMessageBuilder()
                  .initRoot<CollabVmServerMessage>()
                  .initMessage().setChangePasswordResponse(success);
                QueueMessage(std::move(socket_message));
              };
              server_.login_strand_.post(
                std::move(lambda),
                std::allocator<decltype(lambda)>());
            });
          break;
        case CollabVmClientMessage::Message::CHAT_MESSAGE:
        {
          if (is_captcha_required_)
          {
            break;
          }
          const auto chat_message = message.getChatMessage();
          const auto message_len = chat_message.getMessage().size();
          const auto now = std::chrono::steady_clock::now();
          if (!message_len || message_len > Common::max_chat_message_len
              || now - last_chat_message_ < Common::chat_rate_limit)
          {
            break;
          }
          last_chat_message_ = now;
          username_.dispatch(
            [this, self = shared_from_this(),
            buffer = std::move(buffer), chat_message]
          (auto& username)
          {
            if (username.empty())
            {
              return;
            }
            const auto destination =
              chat_message.getDestination().getDestination();
            switch (destination.which())
            {
            case CollabVmClientMessage::ChatMessageDestination::Destination::
            NEW_DIRECT:
              server_.guests_.dispatch([
                  this, self = shared_from_this(), buffer = std::move(buffer),
                  chat_message, username = destination.getNewDirect()
                ](auto& guests) mutable
                {
                  auto guests_it = guests.find(username);
                  if (guests_it == guests.end())
                  {
                    SendChatMessageResponse(
                      CollabVmServerMessage::ChatMessageResponse::
                      USER_NOT_FOUND);
                    return;
                  }
                  chat_rooms_.dispatch([
                      this, self = shared_from_this(), buffer = std::
                      move(buffer),
                      chat_message, recipient = guests_it->second
                    ](auto& chat_rooms)
                    {
                      if (chat_rooms.size() >= 10)
                      {
                        SendChatMessageResponse(
                          CollabVmServerMessage::ChatMessageResponse::
                          USER_CHAT_LIMIT);
                        return;
                      }
                      auto existing_chat_room =
                        std::find_if(chat_rooms.begin(),
                                     chat_rooms.end(),
                                     [&recipient](const auto& room)
                                     {
                                       return room.second.first ==
                                         recipient;
                                     });
                      if (existing_chat_room != chat_rooms.end())
                      {
                        SendChatChannelId(
                          existing_chat_room->second.second);
                        return;
                      }
                      const auto id = chat_rooms_id_++;
                      chat_rooms.emplace(
                        id, std::make_pair(recipient, 0));
                      recipient->chat_rooms_.dispatch([
                          this, self = shared_from_this(),
                          buffer = std::move(buffer),
                          chat_message, recipient, sender_id
                          = id
                        ](auto& recipient_chat_rooms)
                        {
                          auto existing_chat_room = std::
                            find_if(
                              recipient_chat_rooms.begin(),
                              recipient_chat_rooms.end(),
                              [&self](const auto& room)
                              {
                                return room.second.first ==
                                  self;
                              });
                          if (existing_chat_room !=
                            recipient_chat_rooms.end())
                          {
                            if (!existing_chat_room
                                 ->second.second)
                            {
                              existing_chat_room
                                ->second.second = sender_id;
                              return;
                            }
                            SendChatChannelId(sender_id);
                            return;
                          }
                          if (recipient_chat_rooms.size() >=
                            10)
                          {
                            chat_rooms_.dispatch([
                                this, self =
                                shared_from_this(), sender_id
                              ](auto& chat_rooms)
                              {
                                chat_rooms.erase(sender_id);
                                SendChatMessageResponse(
                                  CollabVmServerMessage::
                                  ChatMessageResponse::
                                  RECIPIENT_CHAT_LIMIT);
                              });
                            return;
                          }
                          const auto recipient_id = recipient
                            ->chat_rooms_id_++;
                          recipient_chat_rooms.emplace(
                            recipient_id,
                            std::make_pair(
                              recipient, sender_id));
                          chat_rooms_.dispatch([
                              this, self = shared_from_this()
                              ,
                              buffer = std::move(buffer),
                              chat_message, recipient,
                              sender_id, recipient_id
                            ](auto& chat_rooms)
                            {
                              auto chat_rooms_it = chat_rooms
                                .find(sender_id);
                              if (chat_rooms_it != chat_rooms
                                .end() &&
                                !chat_rooms_it->second.second
                              )
                              {
                                chat_rooms_it->second.second
                                  = recipient_id;
                                SendChatChannelId(sender_id);

                                auto socket_message =
                                  SocketMessage::CreateShared();
                                auto channel_message =
                                  socket_message
                                  ->GetMessageBuilder()
                                  .initRoot<
                                    CollabVmServerMessage>()
                                  .initMessage()
                                  .initNewChatChannel();
                                channel_message.setChannel(
                                  recipient_id);
                                auto message =
                                  channel_message.
                                  initMessage();
                                message.setMessage(
                                  chat_message.getMessage());
                                //                        message.setSender(username);
                                //    message.setTimestamp(timestamp);
                                recipient->QueueMessage(socket_message);
                                QueueMessage(std::move(socket_message));
                              }
                            });
                        });
                    });
                });
              break;
            case CollabVmClientMessage::ChatMessageDestination::Destination::
            DIRECT:
              {
                chat_rooms_.dispatch([
                    this, self = shared_from_this(), username,
                    buffer = std::move(buffer), chat_message, destination
                  ](const auto& chat_rooms) mutable
                  {
                    const auto id = destination.getDirect();
                    const auto chat_rooms_it = chat_rooms.find(id);
                    if (chat_rooms_it == chat_rooms.end())
                    {
                      // TODO: Tell the client the message could not be sent
                      return;
                    }
                    const auto recipient = chat_rooms_it->second.first;
                    recipient->QueueMessage(CreateChatMessage(
                      id, username, chat_message.getMessage()));
                  });
                break;
              }
            case CollabVmClientMessage::ChatMessageDestination::Destination::VM:
              {
                const auto id = destination.getVm();
                auto send_message = [
                    this, self = shared_from_this(), username,
                    buffer = std::move(buffer), chat_message
                  ](auto& channel)
                {
                  auto& chat_room = channel.GetChatRoom();
                  auto new_chat_message = SocketMessage::CreateShared();
                  auto chat_room_message =
                    new_chat_message->GetMessageBuilder()
                                    .initRoot<CollabVmServerMessage>()
                                    .initMessage()
                                    .initChatMessage();
                  chat_room.AddUserMessage(chat_room_message,
                                           username,
                                           GetUserType(),
                                           chat_message.getMessage());
                  channel.BroadcastMessage(std::move(new_chat_message));
                };
                if (id == global_channel_id)
                {
                  server_.global_chat_room_.dispatch(std::move(send_message));
                  break;
                }
              server_.virtual_machines_.dispatch([
                  id, send_message = std::move(send_message)
              ](auto& virtual_machines)
                {
                  const auto virtual_machine = virtual_machines.
                    GetAdminVirtualMachine(id);
                  if (!virtual_machine)
                  {
                    return;
                  }
                  virtual_machine->GetUserChannel(
                    std::move(send_message));
                });
              break;
            }
            default:
              break;
            }
          });
        }
        break;
        case CollabVmClientMessage::Message::VM_LIST_REQUEST:
          {
            server_.virtual_machines_.dispatch(
              [this, self = shared_from_this()](auto& virtual_machines) mutable
              {
                if (!is_viewing_vm_list_)
                {
                  is_viewing_vm_list_ = true;
                  virtual_machines.AddVmListViewer(std::move(self));
                }
              });
            break;
          }
        case CollabVmClientMessage::Message::LOGIN_REQUEST:
          {
            auto login_request = message.getLoginRequest();
            const auto username = login_request.getUsername();
            const auto password = login_request.getPassword();
            const auto captcha_token = login_request.getCaptchaToken();
            server_.captcha_verifier_.Verify(
              captcha_token.cStr(),
              [
                this, self = shared_from_this(),
                buffer = std::move(buffer), username, password
              ](bool is_valid) mutable
              {
                auto socket_message = SocketMessage::CreateShared();
                auto& message_builder = socket_message->
                  GetMessageBuilder();
                auto login_response =
                  message_builder
                  .initRoot<CollabVmServerMessage::Message>()
                  .initLoginResponse()
                  .initResult();
                if (is_valid)
                {
                  auto lambda = [
                      this, self = std::move(self), socket_message,
                      buffer = std::move(buffer), login_response,
                      username,
                      password
                    ]() mutable
                  {
                    const auto [login_result, totp_key] =
                      server_.db_.Login(username, password);
                    if (login_result == CollabVmServerMessage::
                      LoginResponse::
                      LoginResult::SUCCESS)
                    {
                      server_.CreateSession(
                        shared_from_this(), username,
                        [
                          this, self = std::move(self), socket_message,
                          login_response
                        ](const std::string& username,
                          const SessionId& session_id) mutable
                        {
                          auto session = login_response.initSession();
                          session.setSessionId(capnp::Data::Reader(
                            reinterpret_cast<const kj::byte*>(session_id.data()),
                            session_id.size()));
                          session.setUsername(username);
                          session.setIsAdmin(is_admin_);
                          QueueMessage(std::move(socket_message));
                        });
                    }
                    else
                    {
                      if (login_result ==
                        CollabVmServerMessage::LoginResponse::
                        LoginResult::
                        TWO_FACTOR_REQUIRED)
                      {
                        totp_key_ = std::move(totp_key);
                      }
                      login_response.setResult(login_result);
                      QueueMessage(std::move(socket_message));
                    }
                  };
                  server_.login_strand_.post(
                    std::move(lambda),
                    std::allocator<decltype(lambda)>());
                }
                else
                {
                  login_response.setResult(
                    CollabVmServerMessage::LoginResponse::LoginResult::
                    INVALID_CAPTCHA_TOKEN);
                  QueueMessage(std::move(socket_message));
                }
              },
              TSocket::GetIpAddress().AsString());
            break;
          }
        case CollabVmClientMessage::Message::TWO_FACTOR_RESPONSE:
          {
            Totp::ValidateTotp(message.getTwoFactorResponse(),
                               gsl::as_bytes(gsl::make_span(&totp_key_.front(),
                                                            totp_key_.size())));
            break;
          }
        case CollabVmClientMessage::Message::ACCOUNT_REGISTRATION_REQUEST:
          {
            auto create_account =
              [this, self = TSocket::shared_from_this(),
                buffer = std::move(buffer), message]
                (auto& settings) mutable
              {
                auto register_request = message.
                  getAccountRegistrationRequest();
                const auto requested_username = register_request.getUsername();
                const auto invite_id = register_request.getInviteId();
                auto response = SocketMessage::CreateShared();
                auto& message_builder = response->GetMessageBuilder();
                auto registration_result = message_builder.initRoot<
                  CollabVmServerMessage::Message>()
                  .initAccountRegistrationResponse().initResult();
                auto valid_username = std::string();
                if (invite_id.size()) {
                  auto is_valid = false;
                  std::tie(is_valid, valid_username) =
                    server_.db_.ValidateInvite(
                      {reinterpret_cast<const std::byte*>(invite_id.begin()),
                      reinterpret_cast<const std::byte*>(invite_id.end())});
                  if (!is_valid || valid_username.empty() ^ requested_username.size()) {
                    registration_result.setErrorStatus(
                      CollabVmServerMessage::RegisterAccountResponse::
                      RegisterAccountError::INVITE_INVALID);
                    QueueMessage(std::move(response));
                    return;
                  }
                } else if (settings
                     .GetServerSetting(
                       ServerSetting::Setting::
                       ALLOW_ACCOUNT_REGISTRATION)
                     .getAllowAccountRegistration())
                {
                  if (!Common::ValidateUsername({
                      requested_username.begin(), requested_username.size()
                    }))
                    {
                      registration_result.setErrorStatus(
                        CollabVmServerMessage::RegisterAccountResponse::
                        RegisterAccountError::USERNAME_INVALID);
                      QueueMessage(std::move(response));
                      return;
                    }
                  valid_username = requested_username;
                } else {
                  return;
                }
                if (register_request.getPassword().size() >
                  Database::max_password_len)
                {
                  registration_result.setErrorStatus(
                    CollabVmServerMessage::RegisterAccountResponse::
                    RegisterAccountError::PASSWORD_INVALID);
                  QueueMessage(std::move(response));
                  return;
                }
                std::optional<
                  gsl::span<const std::byte, Database::User::totp_key_len>>
                  totp_key;
                if (register_request.hasTwoFactorToken())
                {
                  auto two_factor_token = register_request.
                    getTwoFactorToken();
                  if (two_factor_token.size() !=
                    Database::User::totp_key_len)
                  {
                    registration_result.setErrorStatus(
                      CollabVmServerMessage::RegisterAccountResponse::
                      RegisterAccountError::TOTP_ERROR);
                    QueueMessage(std::move(response));
                    return;
                  }
                  totp_key =
                    gsl::as_bytes(gsl::make_span(
                      reinterpret_cast<const capnp::byte(&)
                      [Database::User::totp_key_len]>(
                        *two_factor_token.begin())));
                }
                std::optional<gsl::span<const std::byte, Database::UserInvite::id_length>> invite_span;
                if (invite_id.size()) {
                    invite_span = std::optional(gsl::as_bytes(gsl::make_span(invite_id.begin(), invite_id.end())));
                }
                const auto register_result = server_.db_.CreateAccount(
                  valid_username,
                  register_request.getPassword(),
                  totp_key,
                  invite_span,
                  TSocket::GetIpAddress().AsVector());
                if (register_result !=
                  CollabVmServerMessage::RegisterAccountResponse::
                  RegisterAccountError::SUCCESS)
                {
                  registration_result.setErrorStatus(register_result);
                  QueueMessage(std::move(response));
                  return;
                }
                server_.CreateSession(
                  shared_from_this(), valid_username,
                  [
                    this, self = shared_from_this(), buffer = std::move(
                      buffer),
                    response, registration_result
                  ](const std::string& username,
                    const SessionId& session_id) mutable
                  {
                    auto session = registration_result.initSession();
                    session.setSessionId(capnp::Data::Reader(
                      reinterpret_cast<const kj::byte*>(session_id.data()),
                      session_id.size()));
                    session.setUsername(username);
                    QueueMessage(std::move(response));
                  });
              };
            auto register_request = message.getAccountRegistrationRequest();
            if (register_request.getInviteId().size()) {
              // Captchas are not required for invites
              server_.settings_.dispatch(std::move(create_account));
              break;
            }
            server_.captcha_verifier_.Verify(
              register_request.getCaptchaToken().cStr(),
              [this, create_account = std::move(create_account)]
              (bool is_valid) mutable
              {
                if (is_valid) {
                  server_.settings_.dispatch(std::move(create_account));
                  return;
                }
                auto response = SocketMessage::CreateShared();
                auto& message_builder = response->GetMessageBuilder();
                auto registration_result = message_builder.initRoot<
                  CollabVmServerMessage::Message>()
                  .initAccountRegistrationResponse().initResult();
                registration_result.setErrorStatus(
                  CollabVmServerMessage::RegisterAccountResponse::
                  RegisterAccountError::INVALID_CAPTCHA_TOKEN);
                QueueMessage(std::move(response));
              });
            break;
          }
        case CollabVmClientMessage::Message::SERVER_CONFIG_REQUEST:
          {
            if (!is_admin_)
            {
              break;
            }
            server_.settings_.dispatch(
              [ this, self = shared_from_this() ](auto& settings)
              {
                QueueMessage(SocketMessage::CopyFromMessageBuilder(
                  settings.GetServerSettingsMessageBuilder()));
              });
            if (!is_viewing_server_config)
            {
              is_viewing_server_config = true;
              server_.virtual_machines_.dispatch([self = shared_from_this()]
                (auto& virtual_machines) mutable
                {
                  virtual_machines.AddAdminVmListViewer(std::move(self));
                });
            }
            break;
          }
        case CollabVmClientMessage::Message::SERVER_CONFIG_MODIFICATIONS:
        {
          if (!is_admin_)
          {
            break;
          }
          auto changed_settings = message.getServerConfigModifications();
          for (const auto& setting_message : changed_settings)
          {
            // TODO: Validate setttings
          }
          server_.settings_.dispatch([
            this, self = shared_from_this(), buffer = std::move(
              buffer),
              changed_settings
          ](auto& settings) mutable
            {
              settings.UpdateServerSettings(changed_settings,
                [this](auto new_settings, auto current_settings) {
                  server_.ApplySettings(new_settings, current_settings);
                });
              auto config_message = SocketMessage::CopyFromMessageBuilder(
                settings.GetServerSettingsMessageBuilder());
              // Broadcast the config changes to all other admins viewing the
              // admin panel
              server_.virtual_machines_.dispatch([
                self = std::move(self),
                config_message = std::move(config_message)
                ]
                (auto& virtual_machines)
                {
                  virtual_machines
                    .BroadcastToViewingAdminsExcluding(config_message, self);
                });
            });
        }
          break;
        case CollabVmClientMessage::Message::SERVER_CONFIG_HIDDEN:
          LeaveServerConfig();
          break;
        case CollabVmClientMessage::Message::CREATE_VM:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch([
              this, self = shared_from_this(),
                buffer = std::move(buffer), message](auto& virtual_machines)
            {
              const auto vm_id = server_.db_.GetNewVmId();
              const auto initial_settings = message.getCreateVm();
              auto& virtual_machine =
                virtual_machines.AddAdminVirtualMachine(
                  server_.GetContext(), vm_id, initial_settings);
              virtual_machine.GetSettings(
                [this, self = std::move(self), vm_id](auto& settings)
                {
                  server_.db_.CreateVm(vm_id, settings.settings_);
                });

              auto socket_message = SocketMessage::CreateShared();
              socket_message->GetMessageBuilder()
                .initRoot<CollabVmServerMessage>().initMessage()
                .setCreateVmResponse(vm_id);
              QueueMessage(socket_message);
              virtual_machines.SendAdminVmList(*this);
            });
          break;
        case CollabVmClientMessage::Message::READ_VMS:
          if (is_admin_)
          {
            server_.virtual_machines_.dispatch([ this,
                self = shared_from_this() ](auto& virtual_machines)
              {
                virtual_machines.SendAdminVmList(*this);
              });
          }
          break;
        case CollabVmClientMessage::Message::READ_VM_CONFIG:
          if (is_admin_)
          {
            const auto vm_id = message.getReadVmConfig();
            server_.virtual_machines_.dispatch([this, self = shared_from_this(),
                vm_id](auto& virtual_machines)
              {
                const auto admin_virtual_machine = virtual_machines
                  .GetAdminVirtualMachine(vm_id);
                if (!admin_virtual_machine)
                {
                  // TODO: Indicate error
                  return;
                }
                admin_virtual_machine->GetSettingsMessage(
                  [this, self = std::move(self)](auto& settings)
                  {
                    QueueMessage(
                      SocketMessage::CopyFromMessageBuilder(settings));
                  });
              });
          }
          break;
        case CollabVmClientMessage::Message::UPDATE_VM_CONFIG:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch(
            [this, self = shared_from_this(), buffer = std::move(buffer),
              message](auto& virtual_machines)
            {
              const auto modified_vm = message.getUpdateVmConfig();
              const auto vm_id = modified_vm.getId();
              const auto virtual_machine =
                virtual_machines.GetAdminVirtualMachine(vm_id);
              if (!virtual_machine)
              {
                return;
              }
              const auto modified_settings = modified_vm.
                getModifications();
              virtual_machine->UpdateSettings(
                server_.db_,
                [buffer = std::move(buffer), modified_settings]() {
                  return modified_settings;
                },
                server_.virtual_machines_.wrap(
                  [self = std::move(self), vm_id]
                  (auto& virtual_machines, auto is_valid_settings) mutable {
                    if (!is_valid_settings) {
                      // TODO: Indicate error
                      return;
                    }
                    const auto virtual_machine =
                      virtual_machines.GetAdminVirtualMachine(vm_id);
                    if (!virtual_machine)
                    {
                      return;
                    }
                    virtual_machines.UpdateVirtualMachineInfo(*virtual_machine);
                  }));
            });
          break;
        case CollabVmClientMessage::Message::DELETE_VM:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch([this, self = shared_from_this(),
              vm_id = message.getDeleteVm()](auto& virtual_machines)
            {
              const auto admin_virtual_machine = virtual_machines
                .RemoveAdminVirtualMachine(vm_id);
              if (!admin_virtual_machine)
              {
                // TODO: Indicate error
                return;
              }
              server_.db_.DeleteVm(vm_id);
              virtual_machines.SendAdminVmList(*this);
            });
          break;
        case CollabVmClientMessage::Message::START_VMS:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch(
            [this, self = shared_from_this(), buffer = std::move(buffer),
              message](auto& virtual_machines)
          {
            for (auto vm_id : message.getStartVms())
            {
              const auto virtual_machine =
                virtual_machines.GetAdminVirtualMachine(vm_id);
              if (!virtual_machine)
              {
                // TODO: Indicate error
                return;
              }
              virtual_machine->Start();
            }
          });
          break;
        case CollabVmClientMessage::Message::STOP_VMS:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch(
            [this, self = shared_from_this(), buffer = std::move(buffer),
              message](auto& virtual_machines)
          {
            for (auto vm_id : message.getStopVms())
            {
              const auto virtual_machine =
                virtual_machines.GetAdminVirtualMachine(vm_id);
              if (!virtual_machine)
              {
                // TODO: Indicate error
                return;
              }
              virtual_machine->Stop();
            }
          });
          break;
        case CollabVmClientMessage::Message::RESTART_VMS:
          if (!is_admin_)
          {
            break;
          }
          server_.virtual_machines_.dispatch(
            [this, self = shared_from_this(), buffer = std::move(buffer),
              message](auto& virtual_machines)
          {
            for (auto vm_id : message.getRestartVms())
            {
              const auto virtual_machine =
                virtual_machines.GetAdminVirtualMachine(vm_id);
              if (!virtual_machine)
              {
                // TODO: Indicate error
                return;
              }
              virtual_machine->Restart();
            }
          });
          break;
        case CollabVmClientMessage::Message::CREATE_INVITE:
          if (is_admin_)
          {
            auto invite = message.getCreateInvite();
            auto socket_message = SocketMessage::CreateShared();
            auto invite_result = socket_message->GetMessageBuilder()
                                               .initRoot<CollabVmServerMessage
                                               >()
                                               .initMessage();
            if (const auto id = server_.db_.CreateInvite(
              {invite.getInviteName().begin(), invite.getInviteName().size()},
              {invite.getUsername().begin(), invite.getUsername().size()},
              invite.getAdmin()))
            {
              invite_result.setCreateInviteResult(
                capnp::Data::Reader(
                  reinterpret_cast<const kj::byte*>(id->data()), id->size()));
            }
            else
            {
              invite_result.initCreateInviteResult(0);
            }
            QueueMessage(std::move(socket_message));
          }
          break;
        case CollabVmClientMessage::Message::READ_INVITES:
          if (is_admin_)
          {
            auto socket_message = SocketMessage::CreateShared();
            auto response = socket_message->GetMessageBuilder()
                                          .initRoot<CollabVmServerMessage>()
                                          .initMessage();
            auto invites_list_it =
              response.initReadInvitesResponse(
                server_.db_.GetInvitesCount()).begin();
            server_.db_.ReadInvites([&invites_list_it](auto&& invite)
            {
              invites_list_it->setId(capnp::Data::Reader(
                reinterpret_cast<const capnp::byte*>(invite.id.data()),
                invite.id.size()));
              invites_list_it->setInviteName(invite.name);
              ++invites_list_it;
            });
            QueueMessage(std::move(socket_message));
          }
          break;
        case CollabVmClientMessage::Message::UPDATE_INVITE:
          if (is_admin_)
          {
            const auto invite = message.getUpdateInvite();
            auto socket_message = SocketMessage::CreateShared();
            const auto invite_id = invite.getId().asChars();
            const auto result = server_.db_.UpdateInvite(
              {reinterpret_cast<const std::byte*>(invite_id.begin()), reinterpret_cast<const std::byte*>(invite_id.end())},
              {invite.getUsername().begin(), invite.getUsername().size()},
              invite.getAdmin());
            socket_message->GetMessageBuilder()
                          .initRoot<CollabVmServerMessage>()
                          .initMessage()
                          .setUpdateInviteResult(result);
            QueueMessage(std::move(socket_message));
          }
          break;
        case CollabVmClientMessage::Message::DELETE_INVITE:
          if (is_admin_)
          {
            const auto invite_id = message.getDeleteInvite();
            server_.db_.DeleteInvite(
              {reinterpret_cast<const std::byte*>(invite_id.begin()), reinterpret_cast<const std::byte*>(invite_id.end())});
          }
          break;
        case CollabVmClientMessage::Message::VALIDATE_INVITE:
        {
            const auto invite_id = message.getValidateInvite();
            const auto invite_id_length = std::distance(reinterpret_cast<const std::byte*>(invite_id.begin()), reinterpret_cast<const std::byte*>(invite_id.end()));
            if (invite_id_length != Database::invite_id_len) {
              break;
            }
            auto [is_valid, username] = server_.db_.ValidateInvite({reinterpret_cast<const std::byte*>(invite_id.begin()), invite_id_length});
            auto socket_message = SocketMessage::CreateShared();
            auto response = socket_message->GetMessageBuilder()
                          .initRoot<CollabVmServerMessage>()
                          .initMessage()
                          .initInviteValidationResponse();
            response.setIsValid(is_valid);
            response.setUsername(username);
            QueueMessage(std::move(socket_message));
            break;
        }
        case CollabVmClientMessage::Message::CREATE_RESERVED_USERNAME:
          if (is_admin_)
          {
            server_.db_.CreateReservedUsername(
              message.getCreateReservedUsername());
          }
          break;
        case CollabVmClientMessage::Message::READ_RESERVED_USERNAMES:
          if (is_admin_)
          {
            auto socket_message = SocketMessage::CreateShared();
            auto response = socket_message->GetMessageBuilder()
                                          .initRoot<CollabVmServerMessage>()
                                          .initMessage();
            auto usernames_list_it =
              response.initReadReservedUsernamesResponse(
                server_.db_.GetReservedUsernamesCount()).begin();
            server_.db_.ReadReservedUsernames(
              [&usernames_list_it](auto username)
              {
                *usernames_list_it = username.data();
              });
            QueueMessage(std::move(socket_message));
          }
          break;
        case CollabVmClientMessage::Message::DELETE_RESERVED_USERNAME:
          if (is_admin_)
          {
            server_.db_.DeleteReservedUsername(
              {message.getDeleteReservedUsername().begin(), message.getDeleteReservedUsername().size()});
          }
          break;
        case CollabVmClientMessage::Message::BAN_IP:
        {
          if (!is_admin_)
          {
            break;
          }
          auto ip_bytes = boost::asio::ip::address_v6::bytes_type();
          *reinterpret_cast<std::uint64_t*>(&ip_bytes[0]) =
            boost::endian::big_to_native(message.getBanIp().getFirst());
          *reinterpret_cast<std::uint64_t*>(&ip_bytes[8]) =
            boost::endian::big_to_native(message.getBanIp().getSecond());
          server_.settings_.dispatch([
            ip_address = boost::asio::ip::address_v6(ip_bytes).to_string()]
            (auto& settings)
            {
              const auto ban_ip_command = 
                settings.GetServerSetting(ServerSetting::Setting::BAN_IP_COMMAND)
                        .getBanIpCommand();
              if (ban_ip_command.size()) {
#ifdef _WIN32
  #define putenv _putenv
#endif
                putenv(("IP_ADDRESS=" + ip_address).data());
#ifdef _WIN32
  #undef putenv
#endif
                ExecuteCommandAsync(ban_ip_command.cStr());
              }
            });
          break;
        }
        case CollabVmClientMessage::Message::SEND_CAPTCHA:
        {
          if (!is_admin_)
          {
            break;
          }
          const auto send_captcha = message.getSendCaptcha();
          const auto username = send_captcha.getUsername();
          server_.GetUser(
            std::string_view(username.cStr(), username.size()),
            send_captcha.getChannel(),
            [buffer = std::move(buffer)](auto& user) {
              auto& [socket, user_data] = user;
              socket->is_captcha_required_ = true;
              auto socket_message = SocketMessage::CreateShared();
              auto& message_builder = socket_message->GetMessageBuilder();
              message_builder.initRoot<CollabVmServerMessage>()
                             .initMessage()
                             .setCaptchaRequired(true);
              socket->QueueMessage(std::move(socket_message));
            });
          break;
        }
        case CollabVmClientMessage::Message::KICK_USER:
        {
          if (!is_admin_)
          {
            break;
          }
          const auto kick_user = message.getKickUser();
          const auto username = kick_user.getUsername();
          server_.GetUser(
            std::string_view(username.cStr(), username.size()),
            kick_user.getChannel(),
            [buffer = std::move(buffer)](auto& user) {
              auto& [socket, user_data] = user;
              socket->Close();
            });
          break;
        }
        case CollabVmClientMessage::Message::PAUSE_TURN_TIMER:
        {
          if (is_admin_ && connected_vm_id_)
          {
            server_.virtual_machines_.dispatch(
              [vm_id = connected_vm_id_](auto& virtual_machines)
              {
                if (const auto virtual_machine =
                      virtual_machines.GetAdminVirtualMachine(vm_id);
                    virtual_machine)
                {
                  virtual_machine->PauseTurnTimer();
                }
              });
          }
          break;
        }
        case CollabVmClientMessage::Message::RESUME_TURN_TIMER:
        {
          if (is_admin_ && connected_vm_id_)
          {
            server_.virtual_machines_.dispatch(
              [vm_id = connected_vm_id_](auto& virtual_machines)
              {
                if (const auto virtual_machine =
                      virtual_machines.GetAdminVirtualMachine(vm_id);
                    virtual_machine)
                {
                  virtual_machine->ResumeTurnTimer();
                }
              });
          }
          break;
        }
        case CollabVmClientMessage::Message::END_TURN:
        {
          if (connected_vm_id_)
          {
            server_.virtual_machines_.dispatch(
              [this, self = shared_from_this(),
                vm_id = connected_vm_id_](auto& virtual_machines) mutable
              {
                if (const auto virtual_machine =
                      virtual_machines.GetAdminVirtualMachine(vm_id);
                    virtual_machine)
                {
                  virtual_machine->EndCurrentTurn(std::move(self));
                }
              });
          }
          break;
        }
        case CollabVmClientMessage::Message::RECORDING_PREVIEW_REQUEST:
        {
          if (is_admin_)
          {
            SendRecordingPreviews(
              std::move(buffer),
              message.getRecordingPreviewRequest());
          }
          break;
        }
        default:
          TSocket::Close();
        }
      }

    private:
      CollabVmServerMessage::UserType GetUserType()
      {
        if (is_admin_) {
          return CollabVmServerMessage::UserType::ADMIN;
        }
        if (is_logged_in_) {
          return CollabVmServerMessage::UserType::REGULAR;
        }
        return CollabVmServerMessage::UserType::GUEST;
      }

      void SendChatChannelId(const std::uint32_t id)
      {
        auto socket_message = SocketMessage::CreateShared();
        auto& message_builder = socket_message->GetMessageBuilder();
        auto message = message_builder.initRoot<CollabVmServerMessage>()
                                      .initMessage()
                                      .initChatMessage();
        message.setChannel(id);
        QueueMessage(std::move(socket_message));
      }

      void SendChatMessageResponse(
        CollabVmServerMessage::ChatMessageResponse result)
      {
        auto socket_message = SocketMessage::CreateShared();
        socket_message->GetMessageBuilder()
                      .initRoot<CollabVmServerMessage>()
                      .initMessage()
                      .setChatMessageResponse(result);
        QueueMessage(std::move(socket_message));
      }

      bool ValidateVmSetting(std::uint16_t setting_id,
                             const VmSetting::Setting::Reader& setting)
      {
        switch (setting_id)
        {
        case VmSetting::Setting::TURN_TIME:
          return setting.getTurnTime() > 0;
        case VmSetting::Setting::DESCRIPTION:
          return setting.getDescription().size() <= 200;
        default:
          return true;
        }
      }

      SessionId SetSessionId(SessionMap& sessions, SessionId&& session_id)
      {
        const auto [session_id_pair, inserted_new] =
          sessions.emplace(std::move(session_id), shared_from_this());
        assert(inserted_new);
        return session_id_pair->first;
      }

      std::shared_ptr<CollabVmSocket> shared_from_this()
      {
        return std::static_pointer_cast<
          CollabVmSocket<typename TServer::TSocket>>(
          TSocket::shared_from_this());
      }

      void SendMessage(std::shared_ptr<CollabVmSocket>&& self,
                       std::shared_ptr<SocketMessage>&& socket_message)
      {
        const auto& segment_buffers = socket_message->
          GetBuffers();
        TSocket::WriteMessage(
          segment_buffers,
          send_queue_.wrap([ this, self = std::move(self), socket_message ](
            auto& send_queue, const auto error_code,
            std::size_t bytes_transferred) mutable
            {
              SendMessageCallback(
                std::move(self), send_queue, error_code, bytes_transferred);
            }));
      }

      void SendMessageBatch(std::shared_ptr<CollabVmSocket>&& self,
                       std::queue<std::shared_ptr<SocketMessage>>& queue)
      {
        auto socket_messages = std::vector<std::shared_ptr<SocketMessage>>();
        socket_messages.reserve(queue.size());
        auto segment_buffers = std::vector<boost::asio::const_buffer>();
        segment_buffers.reserve(queue.size());
        do
        {
          auto& socket_message = *socket_messages.emplace_back(std::move(queue.front()));
          const auto& buffers = socket_message.GetBuffers();
          std::copy(buffers.begin(), buffers.end(), std::back_inserter(segment_buffers));
          queue.pop();
        } while (!queue.empty());

        TSocket::WriteMessage(
          std::move(segment_buffers),
          send_queue_.wrap(
            [ this, self = std::move(self),
            socket_messages = std::move(socket_messages) ](
            auto& send_queue, const auto error_code,
            std::size_t bytes_transferred) mutable
            {
              SendMessageCallback(
                std::move(self), send_queue, error_code, bytes_transferred);
            }));
      }

      void SendMessageCallback(
        std::shared_ptr<CollabVmSocket>&& self,
        std::queue<std::shared_ptr<SocketMessage>>& send_queue,
        const boost::system::error_code error_code,
        std::size_t bytes_transferred)
      {
        if (error_code)
        {
          TSocket::Close();
          return;
        }
        switch (send_queue.size())
        {
        case 0:
          sending_ = false;
          return;
        case 1:
          SendMessage(std::move(self), std::move(send_queue.front()));
          send_queue.pop();
          return;
        default:
          SendMessageBatch(std::move(self), send_queue);
        }
      }
    public:
      template<typename TMessage>
      void QueueMessage(TMessage&& socket_message)
      {
        static_assert(std::is_convertible_v<TMessage, std::shared_ptr<SocketMessage>>);
        socket_message->CreateFrame();
        send_queue_.dispatch([
            this, self = shared_from_this(),
            socket_message =
              std::forward<TMessage>(socket_message)
          ](auto& send_queue) mutable
          {
            if (sending_)
            {
              send_queue.push(std::move(socket_message));
            }
            else
            {
              sending_ = true;
              SendMessage(std::move(self), std::move(socket_message));
            }
          });
      }
      template<typename TCallback>
      void QueueMessageBatch(TCallback&& callback)
      {
        send_queue_.dispatch([
            this, self = shared_from_this(),
            callback = std::forward<TCallback>(callback)
          ](auto& send_queue) mutable
          {
            callback([&send_queue](auto&& socket_message)
            {
              socket_message->CreateFrame();
              send_queue.push(std::forward<decltype(socket_message)>(socket_message));
            });
            if (!send_queue.empty() && !sending_)
            {
              sending_ = true;
              SendMessageBatch(std::move(self), send_queue);
            }
          });
      }
    private:
      void OnDisconnect() override {
        LeaveServerConfig();
        LeaveVmList();
        username_.dispatch(
          [this, self = shared_from_this()](auto& username) {
            if (username.empty()) {
              return;
            }
            server_.guests_.dispatch([
              username = std::move(username)](auto& guests)
              {
                guests.erase(username);
              });
          });
        auto leave_channel =
          [self = shared_from_this()]
          (auto& channel) {
            channel.RemoveUser(std::move(self));
          };
        if (connected_vm_id_) {
          server_.virtual_machines_.dispatch([
            id = connected_vm_id_, leave_channel]
            (auto& virtual_machines)
            {
              const auto virtual_machine = virtual_machines.
                GetAdminVirtualMachine(id);
              if (!virtual_machine)
              {
                return;
              }
              virtual_machine->GetUserChannel(std::move(leave_channel));
            });
        }
        if (is_in_global_chat_) {
          server_.global_chat_room_.dispatch(std::move(leave_channel));
        }
        if (ip_data_) {
          ip_data_->dispatch(
            [this, self = shared_from_this()](auto& ip_data) {
              if (ip_data.connections > 0) {
                --ip_data.connections;
              }
            });
        }
      }

      void LeaveServerConfig()
      {
        if (!is_viewing_server_config)
        {
          return;
        }
        is_viewing_server_config = false;
        server_.virtual_machines_.dispatch([self = shared_from_this()]
          (auto& virtual_machines)
          {
            virtual_machines.RemoveAdminVmListViewer(std::move(self));
          });
      }

      void LeaveVmList()
      {
        if (!is_viewing_vm_list_)
        {
          return;
        }
        is_viewing_vm_list_ = false;
        server_.virtual_machines_.dispatch([self = shared_from_this()]
          (auto& virtual_machines)
          {
            virtual_machines.RemoveVmListViewer(std::move(self));
          });
      }

      void InvalidateSession()
      {
        // TODO:
      }

      static std::shared_ptr<SharedSocketMessage> CreateChatMessage(
        const std::uint32_t channel_id,
        const capnp::Text::Reader sender,
        const capnp::Text::Reader message)
      {
        const auto timestamp =
          std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch())
          .count();
        auto socket_message = SocketMessage::CreateShared();
        auto& message_builder = socket_message->GetMessageBuilder();
        auto channel_chat_message =
          message_builder.initRoot<CollabVmServerMessage>()
                         .initMessage()
                         .initChatMessage();
        channel_chat_message.setChannel(channel_id);
        auto chat_message = channel_chat_message.initMessage();
        chat_message.setMessage(message);
        chat_message.setSender(sender);
        chat_message.setTimestamp(timestamp);
        return socket_message;
      }

      template<typename TContinuation>
      void GenerateUsername(TContinuation&& continuation)
      {
        server_.guests_.dispatch([
            this, continuation = std::forward<TContinuation>(continuation)
        ](auto& guests)
        {
          auto num = server_.guest_rng_(server_.rng_);
          auto username = std::string();
          // Increment the number until a username is found that is not taken
          auto is_username_taken = false;
          do
          {
            if (is_username_taken) {
              num++;
            }
            username = "guest" + std::to_string(num);
            is_username_taken =
              !std::get<bool>(guests.insert({ username, shared_from_this() }));
          } while (is_username_taken);
          SetUserData(username);
          continuation(username);
        });
      }

      template<typename TString>
      void SetUserData(TString&& username) {
        static_assert(std::is_convertible_v<TString, std::string>);
        const auto user_type = GetUserType();
        username_.dispatch(
          [this, self = shared_from_this(), username = std::forward<TString>(username), user_type]
          (auto& current_username) mutable {
            std::swap(current_username, username);
            if (!username.empty() && (connected_vm_id_ || is_in_global_chat_))
            {
              auto update_username = 
                [this, self = shared_from_this(), new_username=current_username, user_type]
                (auto& channel) mutable {
                  auto user_data = channel.GetUserData(self);
                  if (!user_data.has_value()) {
                    return;
                  }
                  user_data->get().user_type = user_type;
                  auto& current_username = user_data.value().get().username;
                  auto message = SocketMessage::CreateShared();
                  auto username_change = message->GetMessageBuilder()
                                                .initRoot<
                                                  CollabVmServerMessage>()
                                                .initMessage()
                                                .initChangeUsername();
                  username_change.setOldUsername(current_username);
                  username_change.setNewUsername(new_username);

                  current_username = std::move(new_username);

                  channel.BroadcastMessage(std::move(message));
                };
              if (connected_vm_id_) {
                server_.virtual_machines_.dispatch([
                  id = connected_vm_id_, update_username]
                  (auto& virtual_machines)
                  {
                    const auto virtual_machine = virtual_machines.
                      GetAdminVirtualMachine(id);
                    if (!virtual_machine)
                    {
                      return;
                    }
                    virtual_machine->GetUserChannel(std::move(update_username));
                  });
              }
              if (is_in_global_chat_) {
                server_.global_chat_room_.dispatch(std::move(update_username));
              }
            }
          });
      }

      void SendRecordingPreviews(
          std::shared_ptr<CollabVmMessageBuffer>&& buffer,
          CollabVmClientMessage::RecordingPreviewRequest::Reader request) {
        const auto sendResult = [this](bool result) {
          auto socket_message = SocketMessage::CreateShared();
          auto& message_builder = socket_message->GetMessageBuilder();
          message_builder.initRoot<CollabVmServerMessage>()
                         .initMessage()
                         .setRecordingPlaybackResult(result);
          QueueMessage(std::move(socket_message));
        };
        if (!request.getStartTime() || !request.getStopTime()) {
          sendResult(false);
          return;
        }
        std::uint64_t current_timestamp = request.getStartTime();
        while (current_timestamp < request.getStopTime()) {
          const auto [file_path, file_start_time, file_stop_time] =
            server_.db_.GetRecordingFilePath(
              request.getVmId(),
              std::chrono::time_point<std::chrono::system_clock>(
                std::chrono::milliseconds(current_timestamp)),
              std::chrono::time_point<std::chrono::system_clock>(
                std::chrono::milliseconds(current_timestamp)));
          if (file_path.empty()) {
            sendResult(false);
            return;
          }
          auto file_stream =
            std::ifstream(file_path, std::ifstream::in | std::ifstream::binary);
          if (!file_stream.is_open()) {
            current_timestamp = 
              std::chrono::duration_cast<std::chrono::milliseconds>(
                file_stop_time.time_since_epoch()).count();
            if (current_timestamp) {
              continue;
            }
            sendResult(false);
            return;
          }
          struct RecordingFileStream {
            std::vector<RecordingFileHeader::Keyframe::Reader> keyframes_;
            capnp::MallocMessageBuilder file_message_builder;
            RecordingFileHeader::Reader file_header;
            std::ifstream file_stream_;
            kj::std::StdInputStream input_stream;
            std::vector<RecordingFileHeader::Keyframe::Reader>::const_iterator keyframe_begin_;
            std::uint64_t current_timestamp;
            RecordingFileStream(std::ifstream&& file_stream)
                : file_stream_(std::forward<std::ifstream>(file_stream)),
                  input_stream(file_stream_) {
              capnp::readMessageCopy(input_stream, file_message_builder);
              file_header = file_message_builder.getRoot<RecordingFileHeader>();
              auto keyframes = file_header.getKeyframes();
              keyframes_.reserve(file_header.getKeyframesCount());
              keyframes_.insert(keyframes_.end(), keyframes.begin(), keyframes.begin() + file_header.getKeyframesCount());
              keyframe_begin_ = keyframes_.cbegin();
              current_timestamp = file_header.getStartTime();
            }
            bool NextKeyframe() {
              if (std::distance(keyframe_begin_, keyframes_.cend()) > 1) {
                SeekToKeyframe(std::next(keyframe_begin_));
                return true;
              }
              current_timestamp = GetNextFileTimestamp();
              return false;
            }
            capnp::MallocMessageBuilder message_builder;
            std::optional<Guacamole::GuacServerInstruction::Reader> ReadGuacamoleInstruction() {
              std::optional<Guacamole::GuacServerInstruction::Reader> guacamole_instruction;
              try {
                CollabVmServerMessage::Message::Reader message;
                do {
                  capnp::readMessageCopy(input_stream, message_builder);
                  message = message_builder.getRoot<CollabVmServerMessage>().getMessage();
                } while (message.which() != CollabVmServerMessage::Message::Which::GUAC_INSTR);
                guacamole_instruction = message.getGuacInstr();
                if (guacamole_instruction->which() == Guacamole::GuacServerInstruction::Which::SYNC) {
                  current_timestamp = guacamole_instruction->getSync();
                }
              } catch (const kj::Exception&) {
                // End of file or deserialization error
              }
              return guacamole_instruction;
            }
            bool SeekToTimestamp(std::uint64_t timestamp) {
              if (timestamp < file_header.getStartTime() || timestamp > file_header.getStopTime()) {
                return false;
              }
              if (timestamp < current_timestamp) {
                keyframe_begin_ = keyframes_.cbegin();
              }
              auto target_keyframe_message_builder = capnp::MallocMessageBuilder();
              auto target_keyframe = target_keyframe_message_builder
                .initRoot<RecordingFileHeader::Keyframe>();
              target_keyframe.setTimestamp(timestamp);
              auto keyframe = std::lower_bound(
                std::make_reverse_iterator(keyframe_begin_),
                keyframes_.crend(),
                target_keyframe,
                [](auto a, auto b) {
                  return a.getTimestamp() > b.getTimestamp();
                });
              if (keyframe != keyframes_.crend()) {
                const auto timestamp_index = keyframe.base() - keyframes_.begin(); //////
                if (current_timestamp < keyframe->getTimestamp()
                    || timestamp < current_timestamp) {
                  SeekToKeyframe(keyframe.base());
                }
              }
              return true;
            }
            [[nodiscard]]
            std::uint64_t GetNextFileTimestamp() {
              return std::max(file_header.getStartTime() + 1, file_header.getStopTime());
            }
          private:
            void SeekToKeyframe(std::vector<RecordingFileHeader::Keyframe::Reader>::const_iterator keyframe) {
              file_stream_.seekg(keyframe->getFileOffset());
              keyframe_begin_ = keyframe;
              current_timestamp = keyframe->getTimestamp();
            }
          };
          try {
            auto recording = RecordingFileStream(std::move(file_stream));
            auto png = std::vector<std::byte>();
            png.reserve(100 * 1'024);
            auto screenshot = GuacamoleScreenshot();
            recording.SeekToTimestamp(current_timestamp);
            auto keyframe_changed = false;
            while (current_timestamp < request.getStopTime()) {
              if (keyframe_changed) {
                screenshot = GuacamoleScreenshot();
                keyframe_changed = false;
              }
              // Create the screenshot from all the frames
              // that occurred before the timestamp
              const auto initial_timestamp = recording.current_timestamp;
              auto one_frame = false;
              do {
                auto message = recording.ReadGuacamoleInstruction();
                if (!message) {
                  break;
                }
                screenshot.WriteInstruction(*message);
                one_frame = true;
              } while ((request.getTimeInterval()
                       && recording.current_timestamp < current_timestamp)
                       || initial_timestamp == recording.current_timestamp);
              if (!one_frame) {
                current_timestamp = recording.GetNextFileTimestamp();
                break;
              }
              png.clear();
              const auto created_screenshot =
                screenshot.CreateScreenshot(
                  request.getWidth(), request.getHeight(),
                  [&png](auto png_bytes) {
                    png.insert(png.end(), png_bytes.begin(), png_bytes.end());
                  });
              auto socket_message = SocketMessage::CreateShared();
              auto& message_builder = socket_message->GetMessageBuilder();
              auto thumbnail_message_builder =
                message_builder.initRoot<CollabVmServerMessage>()
                               .initMessage()
                               .initRecordingPlaybackPreview();
              thumbnail_message_builder.setTimestamp(recording.current_timestamp);
              auto vm_thumbnail = thumbnail_message_builder.initVmThumbnail();
              vm_thumbnail.setId(request.getVmId());
              vm_thumbnail.setPngBytes(kj::ArrayPtr(
                reinterpret_cast<kj::byte*>(png.data()),
                png.size()));
              QueueMessage(std::move(socket_message));

              if (request.getTimeInterval()) {
                current_timestamp = recording.current_timestamp + request.getTimeInterval();
                if (!recording.SeekToTimestamp(current_timestamp)) {
                  break;
                }
              } else {
                const auto next_frame = recording.NextKeyframe();
                current_timestamp = recording.current_timestamp;
                if (!next_frame) {
                  break;
                }
                keyframe_changed = true;
              }
            }
          } catch (...) {
            current_timestamp =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                file_stop_time.time_since_epoch()).count();
            if (!current_timestamp) {
              current_timestamp =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                  file_start_time.time_since_epoch()).count() + 1;
            }
          }
        }
        sendResult(true);
      }

      CollabVmServer& server_;
      StrandGuard<std::queue<std::shared_ptr<SocketMessage>>> send_queue_;
      bool sending_ = false;
      StrandGuard<std::unordered_map<
        std::uint32_t,
        std::pair<std::shared_ptr<CollabVmSocket>, std::uint32_t>>>
        chat_rooms_;
      std::uint32_t chat_rooms_id_ = 1;

      std::vector<std::byte> totp_key_;
      bool is_logged_in_ = false;
      bool is_admin_ = false;
      bool is_viewing_server_config = false;
      bool is_viewing_vm_list_ = false;
      bool is_in_global_chat_ = false;
      bool is_captcha_required_ = false;
      std::chrono::time_point<std::chrono::steady_clock> last_chat_message_;
      std::chrono::time_point<std::chrono::steady_clock> last_username_change_;
      std::uint32_t connected_vm_id_ = 0;
      StrandGuard<std::string> username_;
      std::shared_ptr<StrandGuard<IPData>> ip_data_;
      friend class CollabVmServer;
    };

    using TServer::io_context_;

    CollabVmServer(const std::string& doc_root)
      : TServer(doc_root),
        settings_(io_context_, db_),
        sessions_(io_context_),
        guests_(io_context_),
        ip_data_(io_context_),
        ssl_ctx_(boost::asio::ssl::context::sslv23),
        captcha_verifier_(io_context_, ssl_ctx_),
        virtual_machines_(io_context_,
                          io_context_,
                          db_, *this),
        login_strand_(io_context_),
        global_chat_room_(
          io_context_,
          global_channel_id),
        guest_rng_(1'000, 99'999),
        vm_info_timer_(io_context_)
    {
      settings_.dispatch([this](auto& settings)
      {
				ApplySettings(
          settings.GetServerSettingsMessageBuilder()
					        .getRoot<CollabVmServerMessage>()
                  .getMessage()
                  .getServerSettings());
      });
      StartVmInfoUpdate();
    }

    void StartVmInfoUpdate() {
      vm_info_timer_.expires_after(vm_info_update_frequency_);
      vm_info_timer_.async_wait(
        [&](const auto error_code) {
          if (error_code) {
            return;
          }
          virtual_machines_.dispatch([](auto& virtual_machines)
          {
            virtual_machines.UpdateVirtualMachineInfoList();
          });
          StartVmInfoUpdate();
        });
    }

    void Start(const std::uint8_t threads,
               const std::string& host,
               const std::uint16_t port,
               bool auto_start_vms) {
      if (auto_start_vms)
      {
        virtual_machines_.dispatch([](auto& virtual_machines)
        {
          virtual_machines.ForEachAdminVm([](auto& vm)
          {
            vm.GetSettings([&vm](auto& settings) {
              if (settings.GetSetting(
                    VmSetting::Setting::AUTO_START).getAutoStart())
              {
                vm.Start();
              }
            });
          });
        });
      }
      TServer::Start(threads, host, port);
    }

    void Stop() override {
      vm_info_timer_.cancel();
      virtual_machines_.dispatch(
        [](auto& virtual_machines)
        {
          virtual_machines.ForEachAdminVm(
            [](auto& vm) {
              vm.Stop();
            });
        });
      TServer::Stop();
    }

    static void ExecuteCommandAsync(const std::string_view command) {
      // system() is used for simplicity but it is actually synchronous,
      // so the command is manipulated to make the shell return immediately
      const auto async_shell_command =
#ifdef _WIN32
        std::string("start ") + command.data();
#else
        command.data() + std::string(" &");
#endif
      system(async_shell_command.c_str());
    }

    Database& GetDatabase() {
      return db_;
    }

  protected:
    std::shared_ptr<typename TServer::TSocket> CreateSocket(
      boost::asio::io_context& io_context,
      const std::filesystem::path& doc_root) override
    {
      return std::make_shared<CollabVmSocket<typename TServer::TSocket>>(
        io_context, doc_root, *this);
    }

  private:
    void ApplySettings(const capnp::List<ServerSetting>::Reader settings,
      std::optional<capnp::List<ServerSetting>::Reader> previous_settings = {})
    {
      captcha_verifier_.SetSettings(
        settings[ServerSetting::Setting::CAPTCHA].getSetting().getCaptcha());
      auto recordings_message_builder = std::make_shared<capnp::MallocMessageBuilder>();
      recordings_message_builder->setRoot(
        settings[ServerSetting::Setting::RECORDINGS].getSetting().getRecordings());
      auto recordings_settings =
        recordings_message_builder->getRoot<ServerSetting::Recordings>();
      virtual_machines_.dispatch(
        [this, getRecordingSettings =
          [recordings_settings, message_builder = std::move(recordings_message_builder)]() {
            return recordings_settings;
          }
        ](auto& virtual_machines)
        {
          virtual_machines.ForEachAdminVm(
            [getRecordingSettings = std::move(getRecordingSettings)]
            (auto& vm)
            {
              vm.SetRecordingSettings(getRecordingSettings);
            });
        });
    }

    template <typename TCallback>
    void CreateSession(
      std::shared_ptr<CollabVmSocket<typename TServer::TSocket>>&& socket,
      const std::string& username,
      TCallback&& callback)
    {
      sessions_.dispatch([
          this, socket = std::move(socket), username, callback = std::move(
            callback)
        ](auto& sessions) mutable
        {
          auto [correct_username, is_admin, old_session_id, new_session_id] =
            db_.CreateSession(username, socket->GetIpAddress().AsVector());
          if (correct_username.empty())
          {
            // TODO: Handle error
            return;
          }
          socket->is_logged_in_ = true;
          socket->is_admin_ = is_admin;
          socket->SetUserData(correct_username);
          // TODO: Can SetSessionId return a reference?
          new_session_id =
            socket->SetSessionId(sessions, std::move(new_session_id));
          if (!old_session_id.empty())
          {
            if (auto it = sessions.find(old_session_id);
                it != sessions.end())
            {
              it->second->InvalidateSession();
            }
          }
          callback(correct_username, new_session_id);
        });
    }

    template<typename TCallback>
    void GetChannel(const std::uint32_t id, TCallback&& callback) {
      if (id == global_channel_id)
      {
        global_chat_room_.dispatch(std::forward<TCallback>(callback));
        return;
      }
      virtual_machines_.dispatch(
        [id, callback = std::forward<TCallback>(callback)](auto& virtual_machines) {
          if (const auto virtual_machine = virtual_machines.GetAdminVirtualMachine(id);
              virtual_machine) {
            virtual_machine->GetUserChannel(std::move(callback));
          }
        });
    }

    template<typename TCallback>
    void GetUser(const std::string_view username,
                 const std::uint32_t channel_id,
                 TCallback&& callback) {
      GetChannel(channel_id,
        [username, callback = std::forward<TCallback>(callback)](auto& channel) {
          const auto& users = channel.GetUsers();
          const auto user = std::find_if(users.cbegin(), users.cend(), [username](auto& user) {
            auto& [socket, user_data] = user;
            return user_data.username == username;
          });
          if (user != users.cend()) {
            callback(*user);
          }
        });
    }

    template<typename TCallback>
    void GetIPData(const typename TServer::TSocket::IpAddress& ip_address, TCallback&& callback) {
      ip_data_.dispatch(
        [this, &ip_address, callback = std::forward<TCallback>(callback)]
        (auto& ip_data) {
          auto data = ip_data.find(ip_address.AsBytes());
          callback((data == ip_data.end()
            ? ip_data.try_emplace(ip_address.AsBytes(), std::make_shared<StrandGuard<IPData>>(io_context_)).first
            : data)->second);
        });
    }

    struct ServerSettingsList
    {
      ServerSettingsList(Database& db)
        : db_(db),
          settings_(std::make_unique<capnp::MallocMessageBuilder>()),
          settings_list_(InitSettings(*settings_))
      {
        db_.LoadServerSettings(settings_list_);
      }

      ServerSetting::Setting::Reader GetServerSetting(
        ServerSetting::Setting::Which setting)
      {
        return settings_list_[setting].getSetting().asReader();
      }

      capnp::MallocMessageBuilder& GetServerSettingsMessageBuilder() const
      {
        return *settings_;
      }

      template<typename TCallback>
      void UpdateServerSettings(
        const capnp::List<ServerSetting>::Reader updates,
        TCallback&& callback)
      {
        auto message_builder = std::make_unique<capnp::MallocMessageBuilder>();
        auto new_settings = InitSettings(*message_builder);
        Database::UpdateList<ServerSetting>(settings_list_, new_settings, updates);
        db_.SaveServerSettings(updates);
        auto current_settings = settings_list_;
        settings_list_ = new_settings;
        callback(new_settings, current_settings);
        settings_ = std::move(message_builder);
      }

      static capnp::List<ServerSetting>::Builder InitSettings(
        capnp::MallocMessageBuilder& message_builder)
      {
        const auto fields_count =
          capnp::Schema::from<ServerSetting::Setting>().getUnionFields().size();
        return message_builder.initRoot<CollabVmServerMessage>()
                              .initMessage()
                              .initServerSettings(fields_count);
      }

    private:
      Database& db_;
      std::unique_ptr<capnp::MallocMessageBuilder> settings_;
      capnp::List<ServerSetting>::Builder settings_list_;
    };

    template <typename TClient>
    struct VirtualMachinesList
    {
      VirtualMachinesList(boost::asio::io_context& io_context,
                          Database& db,
                          CollabVmServer& server)
        : server_(server)
      {
        auto admin_vm_list_message_builder = SocketMessage::CreateShared();
        auto admin_virtual_machines =
          std::unordered_map<std::uint32_t, std::shared_ptr<AdminVm>>();
        auto admin_virtual_machines_list =
          admin_vm_list_message_builder->GetMessageBuilder()
                                      .initRoot<CollabVmServerMessage>()
                                      .initMessage()
                                      .initReadVmsResponse(
                                        db.GetVmCount());
        struct VmSettingsList
        {
          capnp::MallocMessageBuilder message_builder_;
          capnp::Orphan<capnp::List<VmSetting>> list = message_builder_.getOrphanage().template newOrphan<capnp::List<VmSetting>>(capnp::Schema::from<VmSetting::Setting>().getUnionFields().size());
          VmSettingsList operator=(VmSettingsList&&) noexcept { return VmSettingsList(); }
        } vm_settings;
        auto previous_vm_id = std::optional<std::size_t>();
        auto vm_setting_index = 0u;
        auto create_vm = [&, admin_vm_info_it = admin_virtual_machines_list.begin()]() mutable
          {
            vm_settings.list.truncate(vm_setting_index);
            vm_setting_index = 0;
            const auto vm_id = previous_vm_id.value();
            admin_virtual_machines.emplace(
              vm_id,
              std::make_shared<AdminVm>(
                io_context, vm_id, server_,
                vm_settings.list.get(), *admin_vm_info_it++)
            );
            vm_settings = VmSettingsList();
          };
        db.ReadVmSettings(
          [&](auto vm_id, auto setting_id, VmSetting::Reader setting) mutable
          {
            if (previous_vm_id.has_value() && previous_vm_id != vm_id)
            {
              create_vm();
            }
            vm_settings.list.get().setWithCaveats(vm_setting_index++, setting);
            previous_vm_id = vm_id;
          });
        if (previous_vm_id.has_value())
        {
          create_vm();
        }
			  admin_vm_info_list_ =
          ResizableList<InitAdminVmInfo>(
            std::move(admin_vm_list_message_builder));
			  admin_virtual_machines_ = std::move(admin_virtual_machines);
      }

      AdminVirtualMachine<CollabVmServer, TClient>* GetAdminVirtualMachine(
        const std::uint32_t id)
      {
        auto vm = admin_virtual_machines_.find(id);
        if (vm == admin_virtual_machines_.end())
        {
          return {};
        }
        return &vm->second->vm;
      }

      bool RemoveAdminVirtualMachine(
        const std::uint32_t id)
      {
        auto vm = admin_virtual_machines_.find(id);
        if (vm == admin_virtual_machines_.end())
        {
          return false;
        }
        vm->second->vm.Stop();
        vm->second->vm.GetUserChannel([](auto& channel) {
          channel.Clear();
        });
        // FIXME: memory leak
        vm->second.reset();
        admin_virtual_machines_.erase(vm);
        admin_vm_info_list_.RemoveFirst([id](auto info)
        {
          return info.getId() == id;
        });
        return true;
      }

      void SendAdminVmList(TClient& client) const
      {
        client.QueueMessage(admin_vm_info_list_.GetMessage());
      }

      template<typename TCallback>
      void ForEachAdminVm(TCallback&& callback)
      {
        for (auto& [id, admin_vm] : admin_virtual_machines_)
        {
          callback(admin_vm->vm);
        }
      }

      /*
      auto AddVirtualMachine(const std::uint32_t id)
      {
        return std::make_shared<VirtualMachine<TClient>>(
          id, vm_info_list_.Add());
      }
      */

      void AddVmListViewer(std::shared_ptr<TClient>&& viewer)
      {
        SendThumbnails(*viewer);
        vm_list_viewers_.emplace_back(
          std::forward<std::shared_ptr<TClient>>(viewer));
      }

      auto& AddAdminVirtualMachine(boost::asio::io_context& io_context,
                                   const std::uint32_t id,
                                   capnp::List<VmSetting>::Reader
                                   initial_settings)
      {
        auto admin_vm_info = admin_vm_info_list_.Add();
        auto vm = std::make_shared<AdminVm>(
          io_context, id, server_, initial_settings, admin_vm_info);
        auto [it, inserted_new] =
          admin_virtual_machines_.emplace(id, std::move(vm));
        assert(inserted_new);
        return it->second->vm;
      }

      void AddAdminVmListViewer(std::shared_ptr<TClient>&& viewer_ptr)
      {
        auto& viewer = *viewer_ptr;
        admin_vm_list_viewers_.emplace_back(
          std::forward<std::shared_ptr<TClient>>(viewer_ptr));
        viewer.QueueMessage(admin_vm_info_list_.GetMessage());
      }

      void BroadcastToViewingAdminsExcluding(
        const std::shared_ptr<CopiedSocketMessage>& message,
        const std::shared_ptr<TClient>& exclude)
      {
        if (admin_vm_list_viewers_.empty() ||
            (admin_vm_list_viewers_.size() == 1 &&
            admin_vm_list_viewers_.front() == exclude))
        {
          return;
        }
        std::for_each(
          admin_vm_list_viewers_.begin(), admin_vm_list_viewers_.end(),
          [&message, &exclude](auto& viewer)
          {
            if (viewer != exclude)
            {
              viewer->QueueMessage(message);
            }
          });
      }

      template<typename TMessage>
      void BroadcastToViewingAdmins(const TMessage& message) {
        std::for_each(
          admin_vm_list_viewers_.begin(), admin_vm_list_viewers_.end(),
          [&message](auto& viewer)
          {
            viewer->QueueMessage(message);
          });
      }

      void RemoveAdminVmListViewer(const std::shared_ptr<TClient>& viewer)
      {
        const auto it = std::find(admin_vm_list_viewers_.begin(),
                                  admin_vm_list_viewers_.end(),
                                  viewer);
        if (it == admin_vm_list_viewers_.end())
        {
          return;
        }
        admin_vm_list_viewers_.erase(it);
      }

      void RemoveVmListViewer(const std::shared_ptr<TClient>& viewer)
      {
        const auto it = std::find(vm_list_viewers_.begin(),
                                  vm_list_viewers_.end(),
                                  viewer);
        if (it == vm_list_viewers_.end())
        {
          return;
        }
        vm_list_viewers_.erase(it);
      }

      std::size_t pending_vm_info_requests_ = 0;
      std::size_t pending_vm_info_updates_  = 0;

      template<typename TFinalizer>
      struct VmInfoProducer {
        VmInfoProducer(TFinalizer&& finalizer)
          : finalizer(std::forward<TFinalizer>(finalizer)) {
        }
        VmInfoProducer(VmInfoProducer&& vm_info_producer) = default;
        TFinalizer finalizer;
        std::vector<std::byte> png_bytes;
        std::unique_ptr<capnp::MallocMessageBuilder> message_builder = std::make_unique<capnp::MallocMessageBuilder>();
        capnp::Orphan<CollabVmServerMessage::AdminVmInfo> admin_vm_info;
        capnp::Orphan<CollabVmServerMessage::VmInfo> vm_info;
        CollabVmServerMessage::AdminVmInfo::Builder InitAdminVmInfo() {
          admin_vm_info = message_builder->getOrphanage().newOrphan<CollabVmServerMessage::AdminVmInfo>();
          return admin_vm_info.get();
        }
        CollabVmServerMessage::VmInfo::Builder InitVmInfo() {
          vm_info = message_builder->getOrphanage().newOrphan<CollabVmServerMessage::VmInfo>();
          return vm_info.get();
        }
        void SetThumbnail(std::vector<std::byte>&& png_bytes) {
          VmInfoProducer::png_bytes =
            std::forward<std::vector<std::byte>>(png_bytes);
        }
        ~VmInfoProducer() {
          if (message_builder) {
            finalizer(*this);
          }
        }
      };

      void UpdateVirtualMachineInfoList()
      {
        if (pending_vm_info_requests_)
        {
          // An update is already pending
          return;
        }
        pending_vm_info_requests_ = admin_virtual_machines_.size();
        pending_vm_info_updates_ = 0;
        for (auto& [vm_id, vm] : admin_virtual_machines_)
        {
          auto callback = server_.virtual_machines_.wrap(
              [this, vm=vm, vm_id=vm_id](auto&, auto& vm_info_producer) mutable
              {
                if (auto& thumbnail_bytes = vm_info_producer.png_bytes;
                  thumbnail_bytes.empty()) {
                } else {
                  thumbnails_.erase(ThumbnailKey("", vm_id));
                  auto& thumbnail_message =
                    thumbnails_[ThumbnailKey("", vm_id)] =
                    SocketMessage::CreateShared();
                  auto& message_builder = thumbnail_message->GetMessageBuilder();
                  auto thumbnail =
                    message_builder.template initRoot<CollabVmServerMessage>()
                    .initMessage().initVmThumbnail();
                  thumbnail.setId(vm_id);
                  thumbnail.setPngBytes(kj::ArrayPtr(
                    reinterpret_cast<kj::byte*>(thumbnail_bytes.data()),
                    thumbnail_bytes.size()));
                }
                vm->has_vm_info = vm_info_producer.vm_info != nullptr;
                if (vm->has_vm_info)
                {
                  pending_vm_info_updates_++;
                }
                vm->SetPendingVmInfo(
                  std::move(vm_info_producer.message_builder),
                  std::move(vm_info_producer.admin_vm_info),
                  std::move(vm_info_producer.vm_info));
                if (--pending_vm_info_requests_)
                {
                  return;
                }
                /*
                // TODO: Sort on server-side
                auto orphanage = admin_vm_info_list_.GetMessageBuilder().getOrphanage();
                orphanage.newOrphan<CollabVmServerMessage::VmInfo>();
                */
                admin_vm_info_list_.Reset(admin_virtual_machines_.size());
                //using GetList = typename decltype(admin_vm_info_list_)::List::GetList;
                auto admin_vm_info_list =
                  decltype(admin_vm_info_list_)::List::GetList(
                    admin_vm_info_list_.GetMessageBuilder());
                vm_info_list_.Reset(pending_vm_info_updates_);
                auto vm_info_list =
                  decltype(vm_info_list_)::List::GetList(
                    vm_info_list_.GetMessageBuilder());
                auto admin_vm_info_list_index = 0u;
                auto vm_info_list_index = 0u;
                for (auto& [id, admin_vm] : admin_virtual_machines_)
                {
                  if (!admin_vm->HasPendingAdminVmInfo())
                  {
                    continue;
                  }
                  admin_vm_info_list.setWithCaveats(
                    admin_vm_info_list_index++,
                    admin_vm->GetPendingAdminVmInfo());
                  if (admin_vm->HasPendingVmInfo())
                  {
                    vm_info_list.setWithCaveats(
                      vm_info_list_index++, admin_vm->GetPendingVmInfo());
                  }
                  admin_vm->FreeVmInfo();
                }
                std::for_each(vm_list_viewers_.begin(), vm_list_viewers_.end(),
                  [vm_list_message=vm_info_list_.GetMessage(),
                   thumbnails=GetThumbnailMessages()](auto& viewer)
                  {
                    viewer->QueueMessageBatch(
                      [vm_list_message, thumbnails](auto queue_message)
                      {
                        queue_message(std::move(vm_list_message));

                        std::for_each(
                          thumbnails->begin(),
                          thumbnails->end(),
                          queue_message);
                      });
                  });
                BroadcastToViewingAdmins(admin_vm_info_list_.GetMessage());
              });
          vm->vm.SetVmInfo(
            VmInfoProducer<decltype(callback)>(std::move(callback)));
        }
      }

      void UpdateVirtualMachineInfo(AdminVirtualMachine<CollabVmServer, TClient>& vm) {
        const auto vm_id = vm.GetId();
	auto callback = server_.virtual_machines_.wrap(
            [this, vm_id](auto&, auto& vm_info_producer) mutable
            {
              auto& vm_data = *admin_virtual_machines_[vm_id];
              if (vm_data.HasPendingAdminVmInfo())
              {
                // A bulk update is already in progress
                vm_data.SetPendingVmInfo(
                  std::move(vm_info_producer.message_builder),
                  std::move(vm_info_producer.admin_vm_info),
                  std::move(vm_info_producer.vm_info));
                return;
              }
              admin_vm_info_list_.UpdateElement(
                [vm_id](auto vm_info)
                {
                  return vm_info.getId() == vm_id;
                }, vm_info_producer.admin_vm_info.get());
              BroadcastToViewingAdmins(admin_vm_info_list_.GetMessage());

              if (vm_data.has_vm_info) {
                auto predicate = [vm_id](auto vm_info)
                  {
                    return vm_info.getId() == vm_id
                      && !vm_info.getHost().size();
                  };
                if (vm_info_producer.vm_info == nullptr) {
                  vm_info_list_.RemoveFirst(std::move(predicate));
                  vm_data.has_vm_info = false;
                } else {
                  vm_info_list_.UpdateElement(
                    std::move(predicate), vm_info_producer.vm_info.get());
                }
              } else {
                if (vm_info_producer.vm_info == nullptr) {
                  return;
                }
                vm_info_list_.Add(vm_info_producer.vm_info.get());
                vm_data.has_vm_info = true;
              }
              std::for_each(vm_list_viewers_.begin(), vm_list_viewers_.end(),
                [vm_list_message=vm_info_list_.GetMessage()](auto& viewer)
                {
                  viewer->QueueMessage(vm_list_message);
                });
            });
        vm.SetVmInfo(
          VmInfoProducer<decltype(callback)>(std::move(callback)));
      }

      template <typename TFunction>
      struct ResizableList
      {
        using List = TFunction;
        explicit ResizableList(std::shared_ptr<SharedSocketMessage>&& message)
          : message_(
              std::forward<std::shared_ptr<SharedSocketMessage>>(message)),
            list_(TFunction::GetList(message_->GetMessageBuilder()))
        {
        }

        ResizableList() :
          message_(SocketMessage::CreateShared()),
          list_(TFunction::InitList(message_->GetMessageBuilder(), 0))
        {
        }

        auto Add()
        {
          auto message = SocketMessage::CreateShared();
          auto vm_list = TFunction::InitList(message->GetMessageBuilder(), list_.size() + 1);
          std::copy(list_.begin(), list_.end(), vm_list.begin());
          list_ = vm_list;
          message_ = std::move(message);
          return vm_list[vm_list.size() - 1];
        }

        template<typename TNewElement>
        void Add(TNewElement new_element)
        {
          auto message = SocketMessage::CreateShared();
          auto vm_list = TFunction::InitList(message->GetMessageBuilder(), list_.size() + 1);
          std::copy(list_.begin(), list_.end(), vm_list.begin());
          list_ = vm_list;
          message_ = std::move(message);
          vm_list.setWithCaveats(vm_list.size() - 1, new_element);
        }

        template<typename TPredicate>
        void RemoveFirst(TPredicate&& predicate)
        {
          auto message = SocketMessage::CreateShared();
          auto vm_list =
            TFunction::InitList(message->GetMessageBuilder(), list_.size() - 1);
          const auto copy_end =
            std::remove_copy_if(list_.begin(), list_.end(),
                                vm_list.begin(), std::move(predicate));
          assert(copy_end == vm_list.end());
          list_ = vm_list;
          message_ = std::move(message);
        }

        template<typename TPredicate, typename TNewElement>
        void UpdateElement(TPredicate&& predicate, TNewElement new_element)
        {
          auto message = SocketMessage::CreateShared();
          const auto size = list_.size();
          auto new_vm_list =
            TFunction::InitList(message->GetMessageBuilder(), size);
          for (auto i = 0u; i < size; i++)
          {
            new_vm_list.setWithCaveats(
              i, predicate(list_[i]) ? new_element : list_[i]);
          }
          list_ = new_vm_list;
          message_ = std::move(message);
        }

        void Reset(unsigned capacity)
        {
          message_ = SocketMessage::CreateShared();
          list_ =
            TFunction::InitList(message_->GetMessageBuilder(), capacity);
        }

        capnp::MessageBuilder& GetMessageBuilder() const
        {
          return message_->GetMessageBuilder();
        }

        std::shared_ptr<SharedSocketMessage> GetMessage() const
        {
          return message_;
        }
      private:
        std::shared_ptr<SharedSocketMessage> message_;
        using ListType = std::invoke_result_t<decltype(TFunction::GetList),
          capnp::MessageBuilder&>;
        ListType list_;
      };

    private:
      struct InitVmInfo
      {
        static capnp::List<CollabVmServerMessage::VmInfo>::Builder GetList(
          capnp::MessageBuilder& message_builder)
        {
          return message_builder
                 .getRoot<CollabVmServerMessage>().getMessage().
                 getVmListResponse();
        }

        static capnp::List<CollabVmServerMessage::VmInfo>::Builder InitList(
          capnp::MessageBuilder& message_builder, unsigned size)
        {
          return message_builder
                 .initRoot<CollabVmServerMessage>().initMessage().
                 initVmListResponse(size);
        }
      };

      struct InitAdminVmInfo
      {
        static capnp::List<CollabVmServerMessage::AdminVmInfo>::Builder GetList(
          capnp::MessageBuilder& message_builder)
        {
          return message_builder
                 .getRoot<CollabVmServerMessage>().getMessage().
                 getReadVmsResponse();
        }

        static capnp::List<CollabVmServerMessage::AdminVmInfo>::Builder InitList(
          capnp::MessageBuilder& message_builder, unsigned size)
        {
          return message_builder
                 .initRoot<CollabVmServerMessage>().initMessage().
                 initReadVmsResponse(size);
        }
      };

      ResizableList<InitVmInfo> vm_info_list_;
      ResizableList<InitAdminVmInfo> admin_vm_info_list_;

      struct AdminVm
      {
        template<typename... TArgs>
        explicit AdminVm(TArgs&& ... args)
          : vm(std::forward<TArgs>(args)...)
        {
        }
	~AdminVm() noexcept { }
        AdminVirtualMachine<CollabVmServer, TClient> vm;
        void SetPendingVmInfo(
            std::unique_ptr<capnp::MallocMessageBuilder>&& message_builder,
            capnp::Orphan<CollabVmServerMessage::AdminVmInfo>&& admin_vm_info,
            capnp::Orphan<CollabVmServerMessage::VmInfo>&& vm_info) {
          pending_admin_vm_info = std::move(admin_vm_info);
          pending_vm_info = std::move(vm_info);
          pending_vm_info_message_builder = std::move(message_builder);
        }
        CollabVmServerMessage::VmInfo::Reader GetPendingVmInfo() const {
          return pending_vm_info.getReader();
        }
        CollabVmServerMessage::AdminVmInfo::Reader GetPendingAdminVmInfo() const {
          return pending_admin_vm_info.getReader();
        }
        bool HasPendingAdminVmInfo() const {
          return pending_admin_vm_info != nullptr;
        }
        bool HasPendingVmInfo() const {
          return pending_vm_info != nullptr;
        }
        void FreeVmInfo() {
          pending_admin_vm_info = {};
          pending_vm_info       = {};
          pending_vm_info_message_builder.reset();
        }
        bool has_vm_info = false;
      private:
        std::unique_ptr<capnp::MallocMessageBuilder> pending_vm_info_message_builder;
        capnp::Orphan<CollabVmServerMessage::AdminVmInfo> pending_admin_vm_info;
        capnp::Orphan<CollabVmServerMessage::VmInfo> pending_vm_info;
      };

      std::shared_ptr<const std::vector<std::shared_ptr<SharedSocketMessage>>>
      GetThumbnailMessages() {
        auto thumbnail_messages =
          std::make_shared<std::vector<std::shared_ptr<SharedSocketMessage>>>();
        thumbnail_messages->reserve(thumbnails_.size());
        std::transform(thumbnails_.begin(), thumbnails_.end(),
          std::back_inserter(*thumbnail_messages),
          [](auto& thumbnail)
          {
              return thumbnail.second;
          });
        return thumbnail_messages;
      }

      void SendThumbnails(TClient& user) {
        user.QueueMessageBatch(
          [vm_list_message=vm_info_list_.GetMessage(),
           thumbnails=GetThumbnailMessages()](auto queue_message)
          {
            queue_message(std::move(vm_list_message));

            std::for_each(
              thumbnails->begin(),
              thumbnails->end(),
              queue_message);
          });
      }

      std::unordered_map<
        std::uint32_t, std::shared_ptr<AdminVm>> admin_virtual_machines_;
      CollabVmServer& server_;
      std::vector<std::shared_ptr<TClient>> vm_list_viewers_;
      std::vector<std::shared_ptr<TClient>> admin_vm_list_viewers_;
      using ThumbnailKey = std::pair<std::string, std::uint32_t>;
      std::unordered_map<ThumbnailKey,
        std::shared_ptr<SharedSocketMessage>,
        boost::hash<ThumbnailKey>> thumbnails_;
    };

    using Socket = CollabVmSocket<typename TServer::TSocket>;

    const std::chrono::seconds vm_info_update_frequency_ =
      std::chrono::seconds(10);

    Database db_;
    StrandGuard<ServerSettingsList> settings_;
    using SessionMap = std::unordered_map<SessionId,
                                          std::shared_ptr<Socket>
                                          >;
    StrandGuard<SessionMap> sessions_;
    StrandGuard<
      std::unordered_map<
        std::string,
        std::shared_ptr<Socket>,
        CaseInsensitiveHasher,
        CaseInsensitiveComparator
      >
    >
    guests_;
    StrandGuard<
      std::unordered_map<
        typename Socket::IpAddress::IpBytes,
        std::shared_ptr<StrandGuard<IPData>>,
        boost::hash<typename Socket::IpAddress::IpBytes>
      >
    > ip_data_;
    boost::asio::ssl::context ssl_ctx_;
    CaptchaVerifier captcha_verifier_;
  public:
    StrandGuard<VirtualMachinesList<CollabVmSocket<typename TServer::TSocket>>>
    virtual_machines_;
    boost::asio::io_context::strand login_strand_;
    StrandGuard<UserChannel<Socket, typename CollabVmSocket<typename TServer::TSocket>::UserData>> global_chat_room_;
    std::uniform_int_distribution<std::uint32_t> guest_rng_;
    std::default_random_engine rng_{std::random_device()()};
    boost::asio::steady_timer vm_info_timer_;
  };
} // namespace CollabVm::Server
