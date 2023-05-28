#include <malloc.h>
#include <poll.h>
#include <unistd.h>
#include <bits/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include "server.h"
#include "../log/log.h"
#include <stdlib.h>
#include <ctype.h>

int get_socket_factory(const char* file_name, int backlog_length, Error** error) {
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        *error = get_error_from_errno("get_socket_factory");
        return -1;
    }

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, file_name, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    bool is_bind_ok = bind(fd, (struct sockaddr*)&addr, sizeof(addr)) >= 0;
    bool is_non_block_ok = fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK)) >= 0;
    bool is_listen_ok = listen(fd, backlog_length) >= 0;

    if (!is_bind_ok || !is_non_block_ok || !is_listen_ok) {
        *error = get_error_from_errno("get_socket_factory");

        if (close(fd) < 0)
            *error = attach_error(get_error_from_errno("get_socket_factory close"), *error);

        return -1;
    }

    return fd;
}

/*
 * Returns connections->length if there is no inactive connections
 * */
ConnectionContext* get_free_cell(const ArrayConnectionContext* connections) {
    int i = 0;
    for (; i < connections->length; i++) {
        if (connections->items[i].socket_fd >= 0)
            continue;

        return &connections->items[i];
    }

    return NULL;
}

void start_receiving(ConnectionContext* connection) {
    connection->request_start_offset = 0;
    connection->request_current_offset = 0;
    connection->state = RECEIVING;
}

void try_accept_connection(ServerState* server_state) {
    struct sockaddr_un addr;
    socklen_t socklen;
    int fd = accept(server_state->socket_factory_fd, (struct sockaddr*)&addr, &socklen);
    if (fd < 0) {
        return;
    }

    if (fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL, 0) | O_NONBLOCK)) < 0) {
        close(fd);
        return;
    }

    ConnectionContext* connection = get_free_cell(&server_state->connections);
    connection->socket_fd = fd;

    start_receiving(connection);
}

void disable_connection(ConnectionContext* connection, Error** error) {
    *error = NULL;

    if (close(connection->socket_fd) < 0) {
        *error = get_error_from_errno("disable_connection");
        return;
    }

    connection->state = DISABLED;
}

void read_into_inner_buffer(ConnectionContext* connection) {
    int requested_count = connection->request_start_offset - connection->request_current_offset;
    if (requested_count == 0)
        return;

    if (requested_count < 0) {
        requested_count = RECEIVE_BUFFER_LENGTH - connection->request_current_offset;
        if (requested_count == 0) {
            connection->request_current_offset = 0;
            return;
        }
    }

    int count = (int)recv(connection->socket_fd,
                          connection->receive_buffer + connection->request_current_offset,
                          requested_count,
                          0);

    if (count <= 0)
        return;

    connection->request_current_offset += count;
}

long parse_long(ConnectionContext* connection, Error** error) {
    *error = NULL;

    long result = 0;
    int i = connection->request_start_offset;
    int length = 0;

    while (connection->receive_buffer[i % RECEIVE_BUFFER_LENGTH] != '\n') {
        if (length > 10) {
            *error = get_error_from_message("Too long request, no line break");
            break;
        }

        char current_char = connection->receive_buffer[i % RECEIVE_BUFFER_LENGTH];
        if (!isdigit(current_char)) {
            *error = get_error_from_message("Request contains non digit chars");
            break;
        }

        result = result * 10 + current_char - '0';

        length++;
        i++;
    }

    connection->request_start_offset = (connection->request_start_offset + length) % RECEIVE_BUFFER_LENGTH;

    return result;
}

void start_sending_response(ConnectionContext* connection, long response) {
    sprintf(connection->send_buffer, "%li\n", response);
    connection->sent_count = 0;
    connection->response_length = (int)strlen(connection->send_buffer);
    connection->state = SENDING;
}

bool try_read(ConnectionContext* connection) {
    if ((connection->poll_revents & POLLIN) == 0)
        return false;

    read_into_inner_buffer(connection);
    return true;
}

void handle_sending(ConnectionContext* connection) {
    if ((connection->poll_revents & POLLOUT) == 0)
        return;

    int count = (int)send(connection->socket_fd, connection->send_buffer + connection->sent_count,
                     connection->response_length - connection->sent_count, 0);

    if (count <= 0)
        return;

    connection->sent_count += count;

    if (connection->sent_count == connection->response_length) {
        log_fmt_msg(INFO, "Sent %s to %d", connection->send_buffer, connection->socket_fd);
        start_receiving(connection);
    }
}

void handle_receiving(ServerState* server_state, ConnectionContext* connection, Error** error) {
    if (!try_read(connection))
        return;

    long request = parse_long(connection, error);
    if (error != NULL)
        return;

    server_state->counter += request;

    start_sending_response(connection, request);
}

void handle_poll_events(ServerState* server_state) {
    Error* error;

    for (int i = 0; i < server_state->connections.length; i++) {
        ConnectionContext* connection = &server_state->connections.items[i];

        if (connection->poll_revents & POLLERR) {
            log_fmt_msg(ERROR, "Socket error fd: '%d'", connection->socket_fd);
            disable_connection(connection, &error);
            if (error != NULL) {
                log_error(error);
                free_error(error);
            }

            continue;
        }

        if (connection->poll_revents & POLLHUP) {
            log_fmt_msg(INFO, "Disconnected fd: '%d'", connection->socket_fd);
            disable_connection(connection, &error);
            if (error != NULL) {
                log_error(error);
                free_error(error);
            }

            continue;
        }

        if (connection->state == SENDING) {
            handle_sending(connection);
            continue;
        }

        if (connection->state == RECEIVING) {
            handle_receiving(server_state, connection, &error);
            if (error != NULL) {
                log_error(error);
                free_error(error);
            }

            continue;
        }

        log_fmt_msg(
                ERROR,
                "[handle_poll_events] poll_revents: '%d' fd: '%d'",
                connection->poll_revents,
                connection->socket_fd);
    }
}

void map_to_poll_fds(ArrayConnectionContext* connections, ArrayPollFd* poll_fds) {
    for (int i = 0; i < connections->length; i++) {
        ConnectionContext* connection = &connections->items[i];

        poll_fds->items[i].fd = connection->state == DISABLED ? -1 : connection->socket_fd;
        poll_fds->items[i].events = POLLIN | POLLOUT;
    }
}

void map_revents(ArrayPollFd* poll_fds, ArrayConnectionContext* connections) {
    for (int i = 0; i < connections->length; i++) {
        ConnectionContext* connection = &connections->items[i];
        connection->poll_revents = poll_fds->items[i].revents;
    }
}

void init_server_state(
        ServerState* server_state,
        const char* socket_file_name,
        int backlog_length,
        Error** error) {
    server_state->counter = 0;
    server_state->is_running = true;

    server_state->socket_factory_fd = get_socket_factory(socket_file_name, backlog_length, error);
    if (*error != NULL)
        return;

    server_state->connections.length = backlog_length;
    server_state->connections.items = calloc(server_state->connections.length, sizeof(ConnectionContext));

    for (int i = 0; i < server_state->connections.length; i++)
        server_state->connections.items[i].state = DISABLED;
}

void run(ServerState* server_state) {
    ArrayPollFd poll_fds;
    poll_fds.length = server_state->connections.length;
    poll_fds.items = calloc(poll_fds.length, sizeof(PollFd));

    while (server_state->is_running) {
        try_accept_connection(server_state);

        map_to_poll_fds(&server_state->connections, &poll_fds);

        int ready_count = poll(poll_fds.items, poll_fds.length, 100);
        if (ready_count < 0) {
            Error* error = get_error_from_errno("poll error");
            log_error(error);
            free_error(error);
        }

        map_revents(&poll_fds, &server_state->connections);

        handle_poll_events(server_state);
    }
}