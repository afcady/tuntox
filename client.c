#include <time.h>

/* MacOS related */
#ifdef __MACH__
#include "mach.h"
#endif

#include "log.h"
#include "main.h"
#include "client.h"

const bool attempt_reconnect = true;

/* The state machine */
int state = CLIENT_STATE_AWAIT_FRIENDSHIP;

/* Used in ping mode */
struct timespec ping_sent_time;

/* Client mode tunnel */
tunnel client_tunnel;

/* Sock representing the local port - call accept() on it */
int bind_sockfd;

fd_set client_master_fdset;

int handle_pong_frame()
{
    struct timespec pong_rcvd_time;
    double secs1, secs2;

    clock_gettime(CLOCK_MONOTONIC, &pong_rcvd_time);

    secs1 = (1.0 * ping_sent_time.tv_sec) + (1e-9 * ping_sent_time.tv_nsec);
    secs2 = (1.0 * pong_rcvd_time.tv_sec) + (1e-9 * pong_rcvd_time.tv_nsec);

    log_printf(L_INFO, "GOT PONG! Time = %.3fs\n", secs2-secs1);

    if(program_mode == Mode_Client_Ping)
    {
        log_printf(L_INFO, "Ping mode successful.  Exiting.");
        exit(0);
    }
    return 0;
}

int local_bind()
{
    struct addrinfo hints, *res;
    char port[6];
    int yes = 1;
    int flags;
    int gai_status;
    int setsockopt_status;

    snprintf(port, 6, "%d", local_port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;  // use IPv4 or IPv6, whichever
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

    gai_status = getaddrinfo(NULL, port, &hints, &res);
    if(gai_status != 0)
    {
        log_printf(L_ERROR, "getaddrinfo: %s\n", gai_strerror(gai_status));
        exit(1);
    }

    bind_sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(bind_sockfd < 0)
    {
        log_printf(L_ERROR, "Could not create a socket for local listening: %s\n", strerror(errno));
        freeaddrinfo(res);
        exit(1);
    }

    setsockopt_status = setsockopt(bind_sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    if(setsockopt_status < 0)
    {
        log_printf(L_ERROR, "Could not set socket options: %s\n", strerror(errno));
        freeaddrinfo(res);
        exit(1);
    }

    /* Set O_NONBLOCK to make accept() non-blocking */
    if (-1 == (flags = fcntl(bind_sockfd, F_GETFL, 0)))
    {
        flags = 0;
    }
    if(fcntl(bind_sockfd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        log_printf(L_ERROR, "Could not make the socket non-blocking: %s\n", strerror(errno));
        freeaddrinfo(res);
        exit(1);
    }

    if(bind(bind_sockfd, res->ai_addr, res->ai_addrlen) < 0)
    {
        log_printf(L_ERROR, "Bind to port %d failed: %s\n", local_port, strerror(errno));
        freeaddrinfo(res);
        close(bind_sockfd);
        exit(1);
    }

    freeaddrinfo(res);

    if(listen(bind_sockfd, 1) < 0)
    {
        log_printf(L_ERROR, "Listening on port %d failed: %s\n", local_port, strerror(errno));
        close(bind_sockfd);
        exit(1);
    }

    log_printf(L_DEBUG, "Bound to local port %d\n", local_port);

    return 0;
}

/* Bind the client.sockfd to a tunnel */
int handle_acktunnel_frame(protocol_frame *rcvd_frame)
{
    tunnel *tun;

    switch (program_mode) {
    case Mode_Client_Local_Port_Forward:
    case Mode_Client_Pipe:
        break;
    default:
        log_printf(L_WARNING, "Got ACKTUNNEL frame in unexpected mode (%d); ignoring", program_mode);
        return -1;
    }

    if(state != CLIENT_STATE_AWAIT_TUNNEL)
    {
        log_printf(L_WARNING, "Got ACKTUNNEL frame in unexpected state (%d); ignoring", state);
        return -1;
    }

    tun = tunnel_create(client_tunnel.sockfd, rcvd_frame->connid, rcvd_frame->friendnumber);
    if (!tun)
    {
        log_printf(L_ERROR, "Got ACKTUNNEL frame but tunnel_create() failed to create new tunnel object");
        exit(1);
    }

    /* Mark that we can accept() another connection */
    client_tunnel.sockfd = -1;

    update_select_nfds(tun->sockfd);
    FD_SET(tun->sockfd, &client_master_fdset);
    state = CLIENT_STATE_CONNECTED;
    return 0;
}

/* Handle a TCP frame received from server */
int handle_server_tcp_frame(protocol_frame *rcvd_frame)
{
    int offset = 0;
    tunnel *tun = NULL;
    int tun_id = rcvd_frame->connid;

    HASH_FIND_INT(by_id, &tun_id, tun);

    if(!tun)
    {
        log_printf(L_WARNING, "Got TCP frame with unknown tunnel ID %d\n", rcvd_frame->connid);
        return -1;
    }

    while(offset < rcvd_frame->data_length)
    {
        int sent_bytes;

        switch (program_mode) {
        case Mode_Client_Pipe:
            sent_bytes = write(1, rcvd_frame->data + offset, rcvd_frame->data_length - offset);
            break;
        case Mode_Client_Local_Port_Forward:
            sent_bytes = send(tun->sockfd, rcvd_frame->data + offset, rcvd_frame->data_length - offset, MSG_NOSIGNAL);
            break;
        default:
            log_printf(L_ERROR, "BUG: Impossible client mode at %s:%s", __FILE__, __LINE__);
            return -1;
        }

        if(sent_bytes < 0)
        {
            uint8_t data[PROTOCOL_BUFFER_OFFSET];
            protocol_frame frame_st, *frame;

            log_printf(L_INFO, "Could not write to socket: %s\n", strerror(errno));

            frame = &frame_st;
            memset(frame, 0, sizeof(protocol_frame));
            frame->friendnumber = tun->friendnumber;
            frame->packet_type = PACKET_TYPE_TCP_FIN;
            frame->connid = tun->connid;
            frame->data_length = 0;
            send_frame(frame, data);
            if(tun->sockfd)
            {
                FD_CLR(tun->sockfd, &client_master_fdset);
            }
            tunnel_delete(tun);

            return -1;
        }

        offset += sent_bytes;
    }

//    printf("Got %d bytes from server - wrote to fd %d\n", rcvd_frame->data_length, tun->sockfd);

    return 0;
}

/* Delete tunnel and clear client-side fdset */
void client_close_tunnel(tunnel *tun)
{
    if(tun->sockfd)
    {
        FD_CLR(tun->sockfd, &client_master_fdset);
    }

    tunnel_delete(tun);
}

/* Handle close-tunnel frame recived from the server */
int handle_server_tcp_fin_frame(protocol_frame *rcvd_frame)
{
    tunnel *tun=NULL;
    int connid = rcvd_frame->connid;

    HASH_FIND_INT(by_id, &connid, tun);

    if(!tun)
    {
        log_printf(L_WARNING, "Got TCP FIN frame with unknown tunnel ID %d\n", rcvd_frame->connid);
        return -1;
    }

    if(tun->friendnumber != rcvd_frame->friendnumber)
    {
        log_printf(L_WARNING, "Friend #%d tried to close tunnel while server is #%d\n", rcvd_frame->friendnumber, tun->friendnumber);
        return -1;
    }

	client_close_tunnel(tun);

    return 0;
}

/* Close and delete all tunnels (when server went offline) */
void client_close_all_connections()
{
	tunnel *tmp = NULL;
	tunnel *tun = NULL;

	HASH_ITER(hh, by_id, tun, tmp)
	{
		client_close_tunnel(tun);
	}
}

void client_connected_loop_iteration(uint32_t friendnumber, struct tox_timer *t)
{
    static unsigned char tox_packet_buf[PROTOCOL_MAX_PACKET_SIZE];
    static fd_set fds;

    int select_rv = 0;
    tunnel *tmp = NULL;
    tunnel *tun = NULL;

    fds = client_master_fdset;

    /* Handle accepting new connections */
    if(client_tunnel.sockfd <= 0) /* Don't accept if we're already waiting to establish a tunnel */
    {
        log_printf(L_DEBUG4, "calling accept(%d)", bind_sockfd);
        int accept_fd = accept(bind_sockfd, NULL, NULL);
        if(accept_fd != -1)
        {
            log_printf(L_INFO, "Accepting a new connection - requesting tunnel...\n");

            /* Open a new tunnel for this FD */
            client_tunnel.sockfd = accept_fd;
            send_tunnel_request_packet(remote_host, remote_port, friendnumber);
        }
    }

    /* Handle reading from sockets */
    select_rv = select(select_nfds, &fds, NULL, NULL, &(t->tv));
    if(select_rv == -1 || select_rv == 0)
    {
        if(select_rv == -1)
        {
            log_printf(L_DEBUG, "Reading from local socket failed: code=%d (%s)\n", errno, strerror(errno));
        }
        else
        {
            log_printf(L_DEBUG4, "Nothing to read...");
        }
    }
    else
    {
        HASH_ITER(hh, by_id, tun, tmp)
        {
            if(FD_ISSET(tun->sockfd, &fds))
            {
                int nbytes;
                if(program_mode == Mode_Client_Local_Port_Forward)
                {
                    nbytes = recv(tun->sockfd,
                            tox_packet_buf + PROTOCOL_BUFFER_OFFSET,
                            READ_BUFFER_SIZE, 0);
                }
                else
                {
                    nbytes = read(tun->sockfd,
                            tox_packet_buf + PROTOCOL_BUFFER_OFFSET,
                            READ_BUFFER_SIZE
                    );
                }

                /* Check if connection closed */
                if(nbytes == 0)
                {
                    uint8_t data[PROTOCOL_BUFFER_OFFSET];
                    protocol_frame frame_st, *frame;

                    log_printf(L_INFO, "Connection closed\n");

                    frame = &frame_st;
                    memset(frame, 0, sizeof(protocol_frame));
                    frame->friendnumber = tun->friendnumber;
                    frame->packet_type = PACKET_TYPE_TCP_FIN;
                    frame->connid = tun->connid;
                    frame->data_length = 0;
                    send_frame(frame, data);
                    if(tun->sockfd)
                    {
                        FD_CLR(tun->sockfd, &client_master_fdset);
                    }
                    tunnel_delete(tun);
                }
                else
                {
                    protocol_frame frame_st, *frame;

                    frame = &frame_st;
                    memset(frame, 0, sizeof(protocol_frame));
                    frame->friendnumber = tun->friendnumber;
                    frame->packet_type = PACKET_TYPE_TCP;
                    frame->connid = tun->connid;
                    frame->data_length = nbytes;
                    send_frame(frame, tox_packet_buf);

//                  printf("Wrote %d bytes from sock %d to tunnel %d\n", nbytes, tun->sockfd, tun->connid);
                }
            }
        }
    }

    fds = client_master_fdset;

    /* Check friend connection status changes */
    if(friend_connection_status == TOX_CONNECTION_NONE)
    {
        log_printf(L_ERROR, "Lost connection to server.  Exiting.");
        exit(1);
    }
}

/* Main loop for the client */
int do_client_loop(uint8_t *tox_id_str)
{
    unsigned char tox_id[TOX_ADDRESS_SIZE];
    uint32_t friendnumber = 0;
    static time_t invitation_sent_time = 0;
    uint32_t invitations_sent = 0;
    TOX_ERR_FRIEND_CUSTOM_PACKET custom_packet_error;

    client_tunnel.sockfd = 0;
    FD_ZERO(&client_master_fdset);

    tox_callback_friend_lossless_packet(tox, parse_lossless_packet);

    if(!string_to_id(tox_id, tox_id_str))
    {
        log_printf(L_ERROR, "Invalid Tox ID");
        exit(1);
    }

    if(program_mode == Mode_Client_Local_Port_Forward)
    {
        local_bind();
        signal(SIGPIPE, SIG_IGN);
    }

    while(1)
    {
        /* Let tox do its stuff */
        tox_iterate(tox, NULL);
        struct tox_timer t = init_tox_timer(tox);

        switch(state)
        {
            /*
             * Send friend request
             */
        case CLIENT_STATE_AWAIT_FRIENDSHIP:
                {
                    uint8_t* data = (uint8_t *)"Hi, fellow tuntox instance!";
                    uint16_t length = sizeof(data);
                    TOX_ERR_FRIEND_ADD add_error;

                    if(use_shared_secret)
                    {
                        data = (uint8_t *)shared_secret;
                        data[TOX_MAX_FRIEND_REQUEST_LENGTH-1] = '\0';
                        length = strlen((char *)data)+1;
                        log_printf(L_DEBUG, "Sent shared secret of length %u\n", length);
                    }

                    if(invitations_sent > 0)
                    {
                        log_printf(L_INFO, "Sending friend request #%d.", invitations_sent+1);
                    }
                    else
                    {
                        log_printf(L_INFO, "Sending friend request.");
                    }

                    friendnumber = tox_friend_add(
                            tox,
                            tox_id,
                            data,
                            length,
                            &add_error
                    );

                    if(add_error != TOX_ERR_FRIEND_ADD_OK)
                    {
                        unsigned char tox_printable_id[TOX_ADDRESS_SIZE * 2 + 1];
                        id_to_string(tox_printable_id, tox_id);
                        log_printf(L_ERROR, "Error %u adding friend %s\n", add_error, tox_printable_id);
                        exit(-1);
                    }

                    invitation_sent_time = time(NULL);
                    invitations_sent++;
                    state = CLIENT_STATE_AWAIT_FRIEND_CONNECTED;
                    log_printf(L_INFO, "Waiting for server to accept friend request...\n");
                }
                break;
            case CLIENT_STATE_AWAIT_FRIEND_CONNECTED:
                if(friend_connection_status != TOX_CONNECTION_NONE)
                {
                    const char* status = readable_connection_status(friend_connection_status);
                    log_printf(L_INFO, "Friend request accepted (%s)!\n", status);

                    switch (program_mode) {
                    case Mode_Client_Ping:
                        /* Send the ping packet */
                        {
                            uint8_t data[] = {
                                0xa2, 0x6a, 0x01, 0x08, 0x00, 0x00, 0x00, 0x05,
                                0x48, 0x65, 0x6c, 0x6c, 0x6f
                            };
                            clock_gettime(CLOCK_MONOTONIC, &ping_sent_time);
                            tox_friend_send_lossless_packet(tox, friendnumber, data, sizeof(data), &custom_packet_error);
                        }
                        if(custom_packet_error != TOX_ERR_FRIEND_CUSTOM_PACKET_OK)
                        {
                            log_printf(L_WARNING, "When sending ping packet: %u", custom_packet_error);
                            exit(1);
                        }
                        state = CLIENT_STATE_AWAIT_PONG;
                        break;
                    case Mode_Client_Local_Port_Forward:
                        if(bind_sockfd < 0)
                        {
                            log_printf(L_ERROR, "Shutting down - could not bind to listening port\n");
                            exit(1);
                        }
                        client_tunnel.sockfd = 0;
                        /* fall through... */
                    case Mode_Client_Pipe:
                        send_tunnel_request_packet(remote_host, remote_port, friendnumber);
                        state = CLIENT_STATE_AWAIT_TUNNEL;
                        break;
                    default:
                        log_printf(L_ERROR, "BUG: Impossible client mode at %s:%s", __FILE__, __LINE__);
                        exit(1);
                    }
                }
                else
                {
                    const int INVITATION_SEND_INTERVAL = 90;
                    if (time(NULL) - invitation_sent_time > INVITATION_SEND_INTERVAL)
                    {
                        TOX_ERR_FRIEND_DELETE error = 0;

                        log_printf(L_INFO, "Friend request timed out.  Sending another...");
                        tox_friend_delete(tox, friendnumber, &error);
                        if(error != TOX_ERR_FRIEND_DELETE_OK)
                        {
                            log_printf(L_ERROR, "Error %u deleting friend before reconnection\n", error);
                            exit(-1);
                        }

                        state = CLIENT_STATE_AWAIT_FRIENDSHIP;
                    }
                }
                break;
            case CLIENT_STATE_AWAIT_TUNNEL:
                break;
            case CLIENT_STATE_AWAIT_PONG:
                break;
            case CLIENT_STATE_CONNECTED:
                client_connected_loop_iteration(friendnumber, &t);
                break;
        }

        run_tox_timer(tox, t);
    }
}

