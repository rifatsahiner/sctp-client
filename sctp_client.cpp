
#include "sctp_client.h"

#include <arpa/inet.h>
#include <netinet/sctp.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#define RECV_BUFFER_SIZE 4096

SctpClient::SctpClient( std::function<void(const std::string&&)> receiveCb, std::function<void(void)> connectionLostCb,
                        std::function<void(const std::string&&)> sendFailCb, std::function<void(void)> peerChangeCb) : _isConnected{false}, _isOpen{false} {
    if(!receiveCb){
        throw std::invalid_argument("receive callback cannot be null");
    }

    _receiveCb = receiveCb;
    _connectionLostCb = connectionLostCb;
    _sendFailCb = sendFailCb;
    _peerChangeCb = peerChangeCb;

    // todo: burada out stream alınarak log atılabilir mi?
    // finalcut logger da dependancy diye bir log sayfası olur, oraya basmak için bu kütüphane gibi dependency'lere verilen stramden gelenler o log sayfasına basılır
}

SctpClient::~SctpClient() {
    if(_isConnected){
        close();
    }
}

////////////////////
/////  public  /////
////////////////////

std::pair<ConnectionResult, std::string_view> SctpClient::connect(const std::string& address, uint8_t port)
{
    if(_isOpen == false) {
        // check and form ip address
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));  // clean struct
        server_addr.sin_family = AF_INET;                   // todo: ipv6 consideration ???
        server_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr) <= 0) {
            // invalid address
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Invalid IP address"));
        }
        
        // open sctp socket
        _sockFd = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
        if (_sockFd < 0) {
            // socket open occurred, return error number and message
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Socket open failed. Error number: " << errno << " Message: " << strerror(errno)));
        }

        // socket subscribe to required events and notifications
        struct sctp_event_subscribe events;
        memset(&events, 0, sizeof(events));     // clean struct
        events.sctp_association_event = 1;      // assoc down
        events.sctp_address_event = 1;          // peer path change
        events.sctp_send_failure_event = 1;     // send failed
        events.sctp_shutdown_event = 1;         // cease send
        if (setsockopt(_sockFd, IPPROTO_SCTP, SCTP_EVENTS, &events, sizeof(events)) < 0) {
            // subs failed, return error number and message
            ::close(_sockFd);
            return std::make_pair(ConnectionResult::SocketError, std::string_view("Socket notification subscribe failed. Error number: " << errno << " Message: " << strerror(errno)));
        }

        _isOpen = true;
    }


    // sctp connect
    if (::connect(_sockFd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        // connect failed
        return std::make_pair(ConnectionResult::PeerConnectError, std::string_view("sctp connect failed. Error number: " << errno << " Message: " << strerror(errno))));
    }

    // start receive thread and return success
    _isConnected = true;
    _receiveThread = std::thread(&SctpClient::_receiveLoop, this);

    return std::make_pair(ConnectionResult::Success, std::string_view());
}

void SctpClient::close(void)
{   
    if(_isConnected) {
        // send shutdown, join receive thread, close socket
        shutdown(_sockFd, SHUT_RDWR);
        _isConnected = false;
        if (_receiveThread.joinable()) {
            _receiveThread.join();
        }
        ::close(_sockFd);
        _isOpen = false;
    }
}

bool SctpClient::send(const std::string& message){
    if(_isConnected == false){
        return false;
    }

    ssize_t bytes_sent = ::send(_sockFd, message.c_str(), message.size(), 0);
    return bytes_sent == static_cast<ssize_t>(message.size());
}

bool SctpClient::sendWithContext(const std::string& message, uint_fast32_t context) {
    struct msghdr msg = {0};
    struct iovec iov;
    struct sctp_sndrcvinfo sndrcv_info;
    struct cmsghdr* cmsg;
    char cmsg_buf[CMSG_SPACE(sizeof(struct sctp_sndrcvinfo))];
    
    if(_isConnected == false){
        return false;
    }
    
    // Set up the payload
    iov.iov_base = const_cast<char*>(message.data());
    iov.iov_len = message.size();
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    // Prepare SCTP sndrcvinfo
    memset(&sndrcv_info, 0, sizeof(sndrcv_info));
    sndrcv_info.sinfo_context = context;  // Set the context value

    // Attach sndrcvinfo to control message
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = IPPROTO_SCTP;
    cmsg->cmsg_type = SCTP_SNDRCV;
    cmsg->cmsg_len = CMSG_LEN(sizeof(sndrcv_info));
    memcpy(CMSG_DATA(cmsg), &sndrcv_info, sizeof(sndrcv_info));

    // send message
    if (sendmsg(_sockFd, &msg, 0) == -1) {
        return false;
    }
    return true;
}

/////////////////////
/////  private  /////
/////////////////////

void SctpClient::_receiveLoop(void) {
    char buffer[RECV_BUFFER_SIZE];

    while(_isConnected) {
        struct sctp_sndrcvinfo sndrcvinfo;
        int flags = 0;

        // Receive messages or notifications
        ssize_t recv_len = sctp_recvmsg(_sockFd, buffer, RECV_BUFFER_SIZE, nullptr, nullptr, &sndrcvinfo, &flags);
        
        if (recv_len < 0) {
            // cannot log, just break
            break;
        }

        // Check if it's a notification
        if (flags & MSG_NOTIFICATION) {
            _handleNotification((union sctp_notification*)buffer, recv_len);
        } else {
            // Handle regular messages
            buffer[recv_len] = '\0';
            _receiveCb(std::string(buffer));
        }
    }
}

void SctpClient::_handleNotification(const union sctp_notification* notif, size_t notifLen)
{
    if (notif == nullptr || notifLen < sizeof(struct sctp_notification)) {
        // cannot log, just return
        return;
    }

    switch (notif->sn_header.sn_type) {
        case SCTP_ASSOC_CHANGE: {
            // const struct sctp_assoc_change *assoc_change = &notif->sn_assoc_change;
            // printf("SCTP_ASSOC_CHANGE: State=%d, Error=%d\n",
            //        assoc_change->sac_state, assoc_change->sac_error);
            break;
        }
        case SCTP_PEER_ADDR_CHANGE: {
            // const struct sctp_paddr_change *paddr_change = &notif->sn_paddr_change;
            // printf("SCTP_PEER_ADDR_CHANGE: Addr=%s, State=%d\n",
            //        inet_ntoa(((struct sockaddr_in*)&paddr_change->spc_aaddr)->sin_addr),
            //        paddr_change->spc_state);
            break;
        }
        case SCTP_SEND_FAILED_EVENT: {
            //
            break;
        }
        case SCTP_SHUTDOWN_EVENT: {
            //
            break;
        }
        default:
            // cannot log, just break
            break;
    }
}
