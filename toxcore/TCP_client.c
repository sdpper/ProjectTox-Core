/*
* TCP_client.c -- Implementation of the TCP relay client part of Tox.
*
*  Copyright (C) 2014 Tox project All Rights Reserved.
*
*  This file is part of Tox.
*
*  Tox is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  Tox is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
*
*/


#include "TCP_client.h"

#if !defined(_WIN32) && !defined(__WIN32__) && !defined (WIN32)
#include <sys/ioctl.h>
#endif

#include "util.h"

/* return 1 on success
 * return 0 on failure
 */
static int connect_sock_to(sock_t sock, IP_Port ip_port)
{
    struct sockaddr_storage addr = {0};
    size_t addrsize;

    if (ip_port.ip.family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;

        addrsize = sizeof(struct sockaddr_in);
        addr4->sin_family = AF_INET;
        addr4->sin_addr = ip_port.ip.ip4.in_addr;
        addr4->sin_port = ip_port.port;
    } else if (ip_port.ip.family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;

        addrsize = sizeof(struct sockaddr_in6);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_addr = ip_port.ip.ip6.in6_addr;
        addr6->sin6_port = ip_port.port;
    } else {
        return 0;
    }

    /* nonblocking socket, connect will never return success */
    connect(sock, (struct sockaddr *)&addr, addrsize);
    return 1;
}
/* return 0 on success.
 * return -1 on failure.
 */
static int generate_handshake(TCP_Client_Connection *TCP_conn, uint8_t *self_public_key, uint8_t *self_secret_key)
{
    uint8_t plain[crypto_box_PUBLICKEYBYTES + crypto_box_NONCEBYTES];
    crypto_box_keypair(plain, TCP_conn->temp_secret_key);
    encrypt_precompute(TCP_conn->public_key, self_secret_key, TCP_conn->shared_key);
    random_nonce(TCP_conn->sent_nonce);
    memcpy(plain + crypto_box_PUBLICKEYBYTES, TCP_conn->sent_nonce, crypto_box_NONCEBYTES);
    memcpy(TCP_conn->last_packet, self_public_key, crypto_box_PUBLICKEYBYTES);
    new_nonce(TCP_conn->last_packet + crypto_box_PUBLICKEYBYTES);
    int len = encrypt_data_fast(TCP_conn->shared_key, TCP_conn->last_packet + crypto_box_PUBLICKEYBYTES, plain,
                                sizeof(plain), TCP_conn->last_packet + crypto_box_PUBLICKEYBYTES + crypto_box_NONCEBYTES);

    if (len != sizeof(plain) + crypto_box_MACBYTES)
        return -1;

    TCP_conn->last_packet_length = crypto_box_PUBLICKEYBYTES + crypto_box_NONCEBYTES + sizeof(plain) + crypto_box_MACBYTES;
    TCP_conn->last_packet_sent = 0;
    return 0;
}

/* data must be of length TCP_SERVER_HANDSHAKE_SIZE
 *
 * return 0 on success.
 * return -1 on failure.
 */
static int handle_handshake(TCP_Client_Connection *TCP_conn, uint8_t *data)
{
    uint8_t plain[crypto_box_PUBLICKEYBYTES + crypto_box_NONCEBYTES];
    int len = decrypt_data_fast(TCP_conn->shared_key, data, data + crypto_box_NONCEBYTES,
                                TCP_SERVER_HANDSHAKE_SIZE - crypto_box_NONCEBYTES, plain);

    if (len != sizeof(plain))
        return -1;

    memcpy(TCP_conn->recv_nonce, plain + crypto_box_PUBLICKEYBYTES, crypto_box_NONCEBYTES);
    encrypt_precompute(plain, TCP_conn->temp_secret_key, TCP_conn->shared_key);
    memset(TCP_conn->temp_secret_key, 0, crypto_box_SECRETKEYBYTES);
    return 0;
}

/* return 0 if pending data was sent completely
 * return -1 if it wasn't
 */
static int send_pending_data(TCP_Client_Connection *con)
{
    if (con->last_packet_length == 0) {
        return 0;
    }

    uint16_t left = con->last_packet_length - con->last_packet_sent;
    int len = send(con->sock, con->last_packet + con->last_packet_sent, left, MSG_NOSIGNAL);

    if (len <= 0)
        return -1;

    if (len == left) {
        con->last_packet_length = 0;
        con->last_packet_sent = 0;
        return 0;
    }

    if (len > left)
        return -1;

    con->last_packet_sent += len;
    return -1;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int write_packet_TCP_secure_connection(TCP_Client_Connection *con, uint8_t *data, uint16_t length)
{
    if (length + crypto_box_MACBYTES > MAX_PACKET_SIZE)
        return -1;

    if (send_pending_data(con) == -1)
        return 0;

    uint8_t packet[sizeof(uint16_t) + length + crypto_box_MACBYTES];

    uint16_t c_length = htons(length + crypto_box_MACBYTES);
    memcpy(packet, &c_length, sizeof(uint16_t));
    int len = encrypt_data_fast(con->shared_key, con->sent_nonce, data, length, packet + sizeof(uint16_t));

    if ((unsigned int)len != (sizeof(packet) - sizeof(uint16_t)))
        return -1;

    increment_nonce(con->sent_nonce);

    len = send(con->sock, packet, sizeof(packet), MSG_NOSIGNAL);

    if ((unsigned int)len == sizeof(packet))
        return 1;

    if (len <= 0)
        return 0;

    memcpy(con->last_packet, packet, length);
    con->last_packet_length = sizeof(packet);
    con->last_packet_sent = len;
    return 1;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int send_routing_request(TCP_Client_Connection *con, uint8_t *public_key)
{
    uint8_t packet[1 + crypto_box_PUBLICKEYBYTES];
    packet[0] = TCP_PACKET_ROUTING_REQUEST;
    memcpy(packet + 1, public_key, crypto_box_PUBLICKEYBYTES);
    return write_packet_TCP_secure_connection(con, packet, sizeof(packet));
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int send_disconnect_notification(TCP_Client_Connection *con, uint8_t id)
{
    uint8_t packet[1 + 1];
    packet[0] = TCP_PACKET_DISCONNECT_NOTIFICATION;
    packet[1] = id;
    return write_packet_TCP_secure_connection(con, packet, sizeof(packet));
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int send_ping_request(TCP_Client_Connection *con, uint64_t ping_id)
{
    uint8_t packet[1 + sizeof(uint64_t)];
    packet[0] = TCP_PACKET_PING;
    memcpy(packet + 1, &ping_id, sizeof(uint64_t));
    return write_packet_TCP_secure_connection(con, packet, sizeof(packet));
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int send_ping_response(TCP_Client_Connection *con, uint64_t ping_id)
{
    uint8_t packet[1 + sizeof(uint64_t)];
    packet[0] = TCP_PACKET_PONG;
    memcpy(packet + 1, &ping_id, sizeof(uint64_t));
    return write_packet_TCP_secure_connection(con, packet, sizeof(packet));
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
int send_onion_request(TCP_Client_Connection *con, uint8_t *data, uint16_t length)
{
    uint8_t packet[1 + length];
    packet[0] = TCP_PACKET_ONION_REQUEST;
    memcpy(packet + 1, data, length);
    return write_packet_TCP_secure_connection(con, packet, sizeof(packet));
}

void onion_response_handler(TCP_Client_Connection *con, int (*onion_callback)(void *object, uint8_t *data,
                            uint16_t length), void *object)
{
    con->onion_callback = onion_callback;
    con->onion_callback_object = object;
}

/* Create new TCP connection to ip_port/public_key
 */
TCP_Client_Connection *new_TCP_connection(IP_Port ip_port, uint8_t *public_key, uint8_t *self_public_key,
        uint8_t *self_secret_key)
{
    if (networking_at_startup() != 0) {
        return NULL;
    }

    if (ip_port.ip.family != AF_INET && ip_port.ip.family != AF_INET6)
        return NULL;

    sock_t sock = socket(ip_port.ip.family, SOCK_STREAM, IPPROTO_TCP);

    if (!sock_valid(sock)) {
        printf("fail1 %u\n", sock);
        return NULL;
    }

    if (!(set_socket_nonblock(sock) && connect_sock_to(sock, ip_port))) {
        kill_sock(sock);
        return NULL;
    }

    TCP_Client_Connection *temp = calloc(sizeof(TCP_Client_Connection), 1);

    if (temp == NULL) {
        kill_sock(sock);
        return NULL;
    }

    temp->status = TCP_CLIENT_CONNECTING;
    temp->sock = sock;
    memcpy(temp->public_key, public_key, crypto_box_PUBLICKEYBYTES);

    if (generate_handshake(temp, self_public_key, self_secret_key) == -1) {
        kill_sock(sock);
        free(temp);
        return NULL;
    }

    temp->kill_at = unix_time() + TCP_CONNECTION_TIMEOUT;

    return temp;
}

/* return 0 on success
 * return -1 on failure
 */
static int handle_TCP_packet(TCP_Client_Connection *conn, uint8_t *data, uint16_t length)
{
    if (length == 0)
        return -1;

    switch (data[0]) {
        case TCP_PACKET_ROUTING_RESPONSE: {
            return 0;
        }

        case TCP_PACKET_CONNECTION_NOTIFICATION: {
            return 0;
        }

        case TCP_PACKET_DISCONNECT_NOTIFICATION: {
            return 0;
        }

        case TCP_PACKET_PING: {
            if (length != 1 + sizeof(uint64_t))
                return -1;

            uint64_t ping_id;
            memcpy(&ping_id, data + 1, sizeof(uint64_t));
            send_ping_response(conn, ping_id);
            return 0;
        }

        case TCP_PACKET_PONG: {
            if (length != 1 + sizeof(uint64_t))
                return -1;

            uint64_t ping_id;
            memcpy(&ping_id, data + 1, sizeof(uint64_t));

            if (ping_id) {
                if (ping_id == conn->ping_id) {
                    conn->ping_id = 0;
                }

                return 0;
            } else {
                return -1;
            }
        }

        case TCP_PACKET_ONION_RESPONSE: {
            conn->onion_callback(conn->onion_callback_object, data + 1, length - 1);
            return 0;
        }

        default: {
            if (data[0] < NUM_RESERVED_PORTS)
                return -1;

            uint8_t con_id = data[0] - NUM_RESERVED_PORTS;
            //TODO
        }
    }

    return 0;
}

static int do_confirmed_TCP(TCP_Client_Connection *conn)
{
    send_pending_data(conn);
    uint8_t packet[MAX_PACKET_SIZE];
    int len;

    if (is_timeout(conn->last_pinged, TCP_PING_FREQUENCY)) {
        uint64_t ping_id = random_64b();

        if (!ping_id)
            ++ping_id;

        int ret = send_ping_request(conn, ping_id);

        if (ret == 1) {
            conn->last_pinged = unix_time();
            conn->ping_id = ping_id;
        }
    }

    if (conn->ping_id && is_timeout(conn->last_pinged, TCP_PING_TIMEOUT)) {
        conn->status = TCP_CLIENT_DISCONNECTED;
        return 0;
    }

    while ((len = read_packet_TCP_secure_connection(conn->sock, &conn->next_packet_length, conn->shared_key,
                  conn->recv_nonce, packet, sizeof(packet)))) {
        if (len == -1) {
            conn->status = TCP_CLIENT_DISCONNECTED;
            break;
        }

        if (handle_TCP_packet(conn, packet, len) == -1) {
            conn->status = TCP_CLIENT_DISCONNECTED;
            break;
        }
    }

    return 0;
}

/* Run the TCP connection
 */
void do_TCP_connection(TCP_Client_Connection *TCP_connection)
{
    unix_time_update();

    if (TCP_connection->status == TCP_CLIENT_DISCONNECTED) {
        return;
    }

    if (TCP_connection->status == TCP_CLIENT_CONNECTING) {
        if (send_pending_data(TCP_connection) == 0) {
            TCP_connection->status = TCP_CLIENT_UNCONFIRMED;
        }
    }

    if (TCP_connection->status == TCP_CLIENT_UNCONFIRMED) {
        uint8_t data[TCP_SERVER_HANDSHAKE_SIZE];
        int len = read_TCP_packet(TCP_connection->sock, data, sizeof(data));

        if (sizeof(data) == len) {
            if (handle_handshake(TCP_connection, data) == 0) {
                TCP_connection->status = TCP_CLIENT_CONFIRMED;
            } else {
                TCP_connection->kill_at = 0;
                TCP_connection->status = TCP_CLIENT_DISCONNECTED;
            }
        }
    }

    if (TCP_connection->status == TCP_CLIENT_CONFIRMED) {
        do_confirmed_TCP(TCP_connection);
    }

    if (TCP_connection->kill_at <= unix_time()) {
        TCP_connection->status = TCP_CLIENT_DISCONNECTED;
    }
}

/* Kill the TCP connection
 */
void kill_TCP_connection(TCP_Client_Connection *TCP_connection)
{
    kill_sock(TCP_connection->sock);
    memset(TCP_connection, 0, sizeof(TCP_Client_Connection));
    free(TCP_connection);
}