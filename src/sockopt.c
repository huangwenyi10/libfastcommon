/*
 * Copyright (c) 2020 YuQing <384681@qq.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the Lesser GNU General Public License, version 3
 * or later ("LGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

//socketopt.c
#include "common_define.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/uio.h>

#define SUB_NET_TYPE_INNER_10_STR2  "inner_10"
#define SUB_NET_TYPE_INNER_172_STR2 "inner_172"
#define SUB_NET_TYPE_INNER_192_STR2 "inner_192"

#define SUB_NET_TYPE_INNER_10_STR3  "inner10"
#define SUB_NET_TYPE_INNER_172_STR3 "inner172"
#define SUB_NET_TYPE_INNER_192_STR3 "inner192"

#if defined(IOV_MAX) && IOV_MAX > 256
#define FC_IOV_BATCH_SIZE  256
#else
#define FC_IOV_BATCH_SIZE  IOV_MAX
#endif

#if defined(OS_LINUX) || defined(OS_FREEBSD)
#include <ifaddrs.h>
#endif

#include <poll.h>
#include <sys/select.h>
#include "shared_func.h"

#ifdef OS_SUNOS
#include <sys/sockio.h>
#endif

#ifdef USE_SENDFILE

#ifdef OS_LINUX
#include <sys/sendfile.h>
#else
#ifdef OS_FREEBSD
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <net/if_dl.h>
#endif
#endif

#endif

#include "logger.h"
#include "hash.h"
#include "sockopt.h"

#ifdef WIN32
#define USE_SELECT
#else
#define USE_POLL
#endif

#ifdef OS_LINUX
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE	 4	/* Start keeplives after this period */
#endif

#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL	 5	/* Interval between keepalives */
#endif

#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 	6	/* Number of keepalives before death */
#endif
#endif

#ifdef OS_LINUX
    bool g_tcp_quick_ack = false;
#endif

static bool try_again_when_interrupt = true;

void tcp_set_try_again_when_interrupt(const bool value)
{
    try_again_when_interrupt = value;
}

void tcp_set_quick_ack(const bool value)
{
#ifdef OS_LINUX
    g_tcp_quick_ack = value;
#endif
}

int tcpgets(int sock, char* s, const int size, const int timeout)
{
	int result;
	char t;
	int i=1;

	if (s == NULL || size <= 0)
	{
		return EINVAL;
	}

	while (i < size)
	{
		result = tcprecvdata(sock, &t, 1, timeout);
		if (result != 0)
		{
			*s = 0;
			return result;
		}

		if (t == '\r')
		{
			continue;
		}

		if (t == '\n')
		{
			*s = t;
			s++;
			*s = 0;
			return 0;
		}

		*s = t;
		s++,i++;
	}

	*s = 0;
	return 0;
}

int tcprecvdata_ex(int sock, void *data, const int size, \
		const int timeout, int *count)
{
	int left_bytes;
	int read_bytes;
	int res;
	int ret_code;
	unsigned char* p;
#ifdef USE_SELECT
	fd_set read_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&read_set);
	FD_SET(sock, &read_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLIN;
#endif

	ret_code = 0;
	p = (unsigned char*)data;
	left_bytes = size;
	while (left_bytes > 0)
	{

#ifdef USE_SELECT
		if (timeout <= 0)
		{
			res = select(sock+1, &read_set, NULL, NULL, NULL);
		}
		else
		{
			t.tv_usec = 0;
			t.tv_sec = timeout;
			res = select(sock+1, &read_set, NULL, NULL, &t);
		}
#else
		res = poll(&pollfds, 1, 1000 * timeout);
		if (res > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
        {
            ret_code = ENOTCONN;
            break;
        }
#endif

		if (res < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			ret_code = errno != 0 ? errno : EINTR;
			break;
		}
		else if (res == 0)
		{
			ret_code = ETIMEDOUT;
			break;
		}
	
		read_bytes = recv(sock, p, left_bytes, 0);
		if (read_bytes < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			ret_code = errno != 0 ? errno : EINTR;
			break;
		}
		if (read_bytes == 0)
		{
			ret_code = ENOTCONN;
			break;
		}

        TCP_SET_QUICK_ACK(sock);
		left_bytes -= read_bytes;
		p += read_bytes;
	}

	if (count != NULL)
    {
        *count = size - left_bytes;
    }

	return ret_code;
}

int tcpsenddata(int sock, void *data, const int size, const int timeout)
{
	int left_bytes;
	int write_bytes;
	int result;
	unsigned char* p;
#ifdef USE_SELECT
	fd_set write_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&write_set);
	FD_SET(sock, &write_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLOUT;
#endif

	p = (unsigned char*)data;
	left_bytes = size;
	while (left_bytes > 0)
	{
#ifdef USE_SELECT
		if (timeout <= 0)
		{
			result = select(sock+1, NULL, &write_set, NULL, NULL);
		}
		else
		{
			t.tv_usec = 0;
			t.tv_sec = timeout;
			result = select(sock+1, NULL, &write_set, NULL, &t);
		}
#else
		result = poll(&pollfds, 1, 1000 * timeout);
		if (result > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
		{
			return ENOTCONN;
		}
#endif

		if (result < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			return errno != 0 ? errno : EINTR;
		}
		else if (result == 0)
		{
			return ETIMEDOUT;
		}

		write_bytes = send(sock, p, left_bytes, 0);
		if (write_bytes < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			return errno != 0 ? errno : EINTR;
		}

		left_bytes -= write_bytes;
		p += write_bytes;
	}

	return 0;
}

int tcprecvdata_nb_ex(int sock, void *data, const int size, \
		const int timeout, int *count)
{
    return tcprecvdata_nb_ms(sock, data, size, timeout * 1000, count);
}

int tcprecvdata_nb_ms(int sock, void *data, const int size, \
		const int timeout_ms, int *count)
{
	int left_bytes;
	int read_bytes;
	int res;
	int ret_code;
	unsigned char* p;
#ifdef USE_SELECT
	fd_set read_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&read_set);
	FD_SET(sock, &read_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLIN;
#endif

	ret_code = 0;
	p = (unsigned char*)data;
	left_bytes = size;
	while (left_bytes > 0)
	{
		read_bytes = recv(sock, p, left_bytes, 0);
		if (read_bytes > 0)
		{
            TCP_SET_QUICK_ACK(sock);
			left_bytes -= read_bytes;
            if (left_bytes == 0)
            {
                break;
            }

			p += read_bytes;
			continue;
		}

		if (read_bytes < 0)
		{
			if (!(errno == EAGAIN || errno == EWOULDBLOCK ||
                        (errno == EINTR && try_again_when_interrupt)))
			{
				ret_code = errno != 0 ? errno : EINTR;
				break;
			}
		}
		else
		{
			ret_code = ENOTCONN;
			break;
		}

#ifdef USE_SELECT
		if (timeout_ms <= 0)
		{
			res = select(sock+1, &read_set, NULL, NULL, NULL);
		}
		else
		{
			t.tv_usec = (timeout_ms % 1000) * 1000;
			t.tv_sec = timeout_ms / 1000;
			res = select(sock+1, &read_set, NULL, NULL, &t);
		}
#else
		res = poll(&pollfds, 1, timeout_ms);
		if (res > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
		{
			ret_code = ENOTCONN;
			break;
		}
#endif

		if (res < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			ret_code = errno != 0 ? errno : EINTR;
			break;
		}
		else if (res == 0)
		{
			ret_code = ETIMEDOUT;
			break;
		}
	}

	if (count != NULL)
	{
		*count = size - left_bytes;
	}

	return ret_code;
}

int tcpreadv_nb_ms(int sock, const int size, const struct iovec *iov,
        const int iovcnt, const int timeout_ms, int *total_bytes)
{
	int left_bytes;
	int read_bytes;
    int bytes;
	int res;
	int ret_code;
    int remain_count;
    int current_count;
    int current_done;
    int remain_len;
    struct iovec *iob;
    struct iovec iov_array[FC_IOV_BATCH_SIZE];
    struct iovec *iovp;

#ifdef USE_SELECT
	fd_set read_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&read_set);
	FD_SET(sock, &read_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLIN;
#endif

	ret_code = 0;
    iovp = (struct iovec *)iov;
    remain_count = current_count = iovcnt;
	left_bytes = size;
	while (left_bytes > 0)
	{
		read_bytes = readv(sock, iovp, current_count);
		if (read_bytes > 0)
		{
            TCP_SET_QUICK_ACK(sock);
			left_bytes -= read_bytes;
            if (left_bytes <= 0)
            {
                break;
            }

            iob = iovp;
            bytes = iob->iov_len;
            while (bytes < read_bytes)
            {
                ++iob;
                bytes += iob->iov_len;
            }
            if (bytes == read_bytes)
            {
                ++iob;
                if (iob < iovp + current_count) {
                    bytes += iob->iov_len;
                }
            }

            current_done = iob - iovp;
            remain_count -= current_done;
            if (remain_count == 0) {
                ret_code = EOVERFLOW;
                break;
            }

            if (current_done == current_count)  //full done
            {
                current_count = ((remain_count <= FC_IOV_BATCH_SIZE) ?
                        remain_count : FC_IOV_BATCH_SIZE);
                memcpy(iov_array, iov + (iovcnt - remain_count),
                        current_count * sizeof(struct iovec));
                iovp = iov_array;
            }
            else  //partial done
            {
                if (iovp == (struct iovec *)iov)
                {
                    current_count = ((remain_count <= FC_IOV_BATCH_SIZE) ?
                            remain_count : FC_IOV_BATCH_SIZE);
                    memcpy(iov_array, iob, current_count *
                            sizeof(struct iovec));
                    iovp = iov_array;
                }
                else
                {
                    current_count -= current_done;
                    iovp = iob;
                }

                /* adjust the first element */
                remain_len = bytes - read_bytes;
                if (remain_len < iovp->iov_len)
                {
                    iovp->iov_base = (char *)iovp->iov_base +
                        (iovp->iov_len - remain_len);
                    iovp->iov_len = remain_len;
                }
            }

			continue;
		}
		else if (read_bytes == 0)
        {
            ret_code = ENOTCONN;
            break;
        }

        if (!(errno == EAGAIN || errno == EWOULDBLOCK ||
                    (errno == EINTR && try_again_when_interrupt)))
        {
            ret_code = errno != 0 ? errno : EINTR;
            break;
        }

#ifdef USE_SELECT
		if (timeout_ms <= 0)
		{
			res = select(sock+1, &read_set, NULL, NULL, NULL);
		}
		else
		{
			t.tv_usec = (timeout_ms % 1000) * 1000;
			t.tv_sec = timeout_ms / 1000;
			res = select(sock+1, &read_set, NULL, NULL, &t);
		}
#else
		res = poll(&pollfds, 1, timeout_ms);
		if (res > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
		{
			ret_code = ENOTCONN;
			break;
		}
#endif

		if (res < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			ret_code = errno != 0 ? errno : EINTR;
			break;
		}
		else if (res == 0)
		{
			ret_code = ETIMEDOUT;
			break;
		}
	}

	if (total_bytes != NULL)
	{
		*total_bytes = size - left_bytes;
	}

	return ret_code;
}

int tcpsenddata_nb(int sock, void *data, const int size, const int timeout)
{
	int left_bytes;
	int write_bytes;
	int result;
	unsigned char *p;
#ifdef USE_SELECT
	fd_set write_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&write_set);
	FD_SET(sock, &write_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLOUT;
#endif

	p = (unsigned char *)data;
	left_bytes = size;
	while (left_bytes > 0)
	{
		write_bytes = send(sock, p, left_bytes, 0);
		if (write_bytes > 0)
		{
			left_bytes -= write_bytes;
            if (left_bytes == 0)
            {
                break;
            }

			p += write_bytes;
			continue;
		}
		else if (write_bytes == 0)
        {
            return ENOTCONN;
        }

        if (!(errno == EAGAIN || errno == EWOULDBLOCK ||
                    (errno == EINTR && try_again_when_interrupt)))
        {
            return errno != 0 ? errno : EINTR;
        }

#ifdef USE_SELECT
		if (timeout <= 0)
		{
			result = select(sock+1, NULL, &write_set, NULL, NULL);
		}
		else
		{
			t.tv_usec = 0;
			t.tv_sec = timeout;
			result = select(sock+1, NULL, &write_set, NULL, &t);
		}
#else
		result = poll(&pollfds, 1, 1000 * timeout);
		if (result > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
		{
			return ENOTCONN;
		}
#endif

		if (result < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			return errno != 0 ? errno : EINTR;
		}
		else if (result == 0)
		{
			return ETIMEDOUT;
		}
	}

	return 0;
}

int tcpwritev_nb(int sock, const struct iovec *iov,
        const int iovcnt, const int timeout)
{
	int write_bytes;
	int bytes;
	int result;
    int remain_count;
    int current_count;
    int current_done;
    int remain_len;
    struct iovec *iob;
    struct iovec iov_array[FC_IOV_BATCH_SIZE];
    struct iovec *iovp;

#ifdef USE_SELECT
	fd_set write_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&write_set);
	FD_SET(sock, &write_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLOUT;
#endif

    iovp = (struct iovec *)iov;
    remain_count = current_count = iovcnt;
	while (remain_count > 0)
	{
		write_bytes = writev(sock, iovp, current_count);
		if (write_bytes > 0)
		{
            iob = iovp;
            bytes = iob->iov_len;
            while (bytes < write_bytes)
            {
                ++iob;
                bytes += iob->iov_len;
            }
            if (bytes == write_bytes)
            {
                ++iob;
                if (iob < iovp + current_count) {
                    bytes += iob->iov_len;
                }
            }

            current_done = iob - iovp;
            remain_count -= current_done;
            if (remain_count == 0) {
                break;
            }

            if (current_done == current_count)  //full done
            {
                current_count = ((remain_count <= FC_IOV_BATCH_SIZE) ?
                        remain_count : FC_IOV_BATCH_SIZE);
                memcpy(iov_array, iov + (iovcnt - remain_count),
                        current_count * sizeof(struct iovec));
                iovp = iov_array;
            }
            else  //partial done
            {
                if (iovp == (struct iovec *)iov)
                {
                    current_count = ((remain_count <= FC_IOV_BATCH_SIZE) ?
                            remain_count : FC_IOV_BATCH_SIZE);
                    memcpy(iov_array, iob, current_count *
                            sizeof(struct iovec));
                    iovp = iov_array;
                }
                else
                {
                    current_count -= current_done;
                    iovp = iob;
                }

                /* adjust the first element */
                remain_len = bytes - write_bytes;
                if (remain_len < iovp->iov_len)
                {
                    iovp->iov_base = (char *)iovp->iov_base +
                        (iovp->iov_len - remain_len);
                    iovp->iov_len = remain_len;
                }
            }

			continue;
		}
		else if (write_bytes == 0)
        {
            return ENOTCONN;
        }

        if (!(errno == EAGAIN || errno == EWOULDBLOCK ||
                    (errno == EINTR && try_again_when_interrupt)))
        {
            return errno != 0 ? errno : EINTR;
        }

#ifdef USE_SELECT
		if (timeout <= 0)
		{
			result = select(sock+1, NULL, &write_set, NULL, NULL);
		}
		else
		{
			t.tv_usec = 0;
			t.tv_sec = timeout;
			result = select(sock+1, NULL, &write_set, NULL, &t);
		}
#else
		result = poll(&pollfds, 1, 1000 * timeout);
		if (result > 0 && (pollfds.revents & (POLLHUP | POLLERR)))
		{
			return ENOTCONN;
		}
#endif

		if (result < 0)
		{
            if (errno == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			return errno != 0 ? errno : EINTR;
		}
		else if (result == 0)
		{
			return ETIMEDOUT;
		}
	}

	return 0;
}

int setsockaddrbyip(const char *ip, const uint16_t port, sockaddr_convert_t *convert)
{
    int af;
    void *dest;

    if (is_ipv6_addr(ip))
    {
        convert->len = sizeof(convert->sa.addr6);
        dest = &convert->sa.addr6.sin6_addr;

        af = AF_INET6;
        convert->sa.addr6.sin6_family = PF_INET6;
        convert->sa.addr6.sin6_port = htons(port);
    }
    else  //ipv4
    {
        convert->len = sizeof(convert->sa.addr4);
        dest = &convert->sa.addr4.sin_addr;

        af = AF_INET;
        convert->sa.addr4.sin_family = PF_INET;
        convert->sa.addr4.sin_port = htons(port);
    }

    if (inet_pton(af, ip, dest) == 0)
    {
		logError("file: "__FILE__", line: %d, "
			"invalid %s ip address: %s", __LINE__,
            (af == AF_INET ? "IPv4" : "IPv6"), ip);
        return EINVAL;
    }
    return 0;
}

int connectserverbyip(int sock, const char *server_ip, const uint16_t server_port)
{
    int result;
    sockaddr_convert_t convert;

    if ((result=setsockaddrbyip(server_ip, server_port, &convert)) != 0)
    {
        return result;
    }

	if (connect(sock, &convert.sa.addr, convert.len) < 0)
	{
		return errno != 0 ? errno : EINTR;
	}

	return 0;
}

int connectserverbyip_nb_ex(int sock, const char *server_ip, \
		const uint16_t server_port, const int timeout, \
		const bool auto_detect)
{
	int result;
	int flags;
	bool needRestore;
	socklen_t len;

#ifdef USE_SELECT
	fd_set rset;
	fd_set wset;
	struct timeval tval;
#else
	struct pollfd pollfds;
#endif

    sockaddr_convert_t convert;

    if ((result=setsockaddrbyip(server_ip, server_port, &convert)) != 0)
    {
        return result;
    }

	if (auto_detect)
	{
		flags = fcntl(sock, F_GETFL, 0);
		if (flags < 0)
		{
			return errno != 0 ? errno : EACCES;
		}

		if ((flags & O_NONBLOCK) == 0)
		{
			if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
			{
				return errno != 0 ? errno : EACCES;
			}

			needRestore = true;
		}
		else
		{
			needRestore = false;
		}
	}
	else
	{
		needRestore = false;
		flags = 0;
	}

	do
	{
		if (connect(sock, &convert.sa.addr, convert.len) < 0)
		{
			result = errno != 0 ? errno : EINPROGRESS;
			if (result != EINPROGRESS)
			{
				break;
			}
		}
		else
		{
			result = 0;
			break;
		}


#ifdef USE_SELECT
		FD_ZERO(&rset);
		FD_ZERO(&wset);
		FD_SET(sock, &rset);
		FD_SET(sock, &wset);
		tval.tv_sec = timeout;
		tval.tv_usec = 0;
		
		result = select(sock+1, &rset, &wset, NULL, \
				timeout > 0 ? &tval : NULL);
#else
		pollfds.fd = sock;
		pollfds.events = POLLIN | POLLOUT;
		result = poll(&pollfds, 1, 1000 * timeout);
#endif

		if (result == 0)
		{
			result = ETIMEDOUT;
			break;
		}
		else if (result < 0)
		{
			result = errno != 0 ? errno : EINTR;
			break;
		}

		len = sizeof(result);
		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &result, &len) < 0)
		{
			result = errno != 0 ? errno : EACCES;
			break;
		}
	} while (0);

	if (needRestore)
	{
		fcntl(sock, F_SETFL, flags);
	}
  
	return result;
}

int asyncconnectserverbyip(int sock, const char *server_ip,
        const uint16_t server_port)
{
    int result;
    sockaddr_convert_t convert;

    if ((result=setsockaddrbyip(server_ip, server_port, &convert)) != 0)
    {
        return result;
    }

    if (connect(sock, &convert.sa.addr, convert.len) == 0) {
        return 0;
    }
    else
    {
        return errno != 0 ? errno : EINPROGRESS;
    }
}

int socketCreateEx2(int af, const char *server_ip,
		const int flags, const char *bind_ipaddr, int *err_no)
{
    int sock;

    if (!(af == AF_INET || af == AF_INET6))
    {
        af = is_ipv6_addr(server_ip) ? AF_INET6 : AF_INET;
    }

    sock = socket(af, SOCK_STREAM, 0);
    if (sock < 0)
    {
        *err_no = errno != 0 ? errno : EMFILE;
        logError("file: "__FILE__", line: %d, "
                "socket create failed, errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
        return -1;
    }

    FC_SET_CLOEXEC(sock);
    SET_SOCKOPT_NOSIGPIPE(sock);
    if (flags != 0)
    {
        *err_no = fd_add_flags(sock, flags);
        if (*err_no != 0)
        {
            close(sock);
            return -2;
        }
    }

    if (bind_ipaddr != NULL && *bind_ipaddr != '\0')
    {
        *err_no = socketBind2(af, sock, bind_ipaddr, 0);
        if (*err_no != 0)
        {
            close(sock);
            return -3;
        }
    }

    *err_no = 0;
    return sock;
}

int socketClientEx2(int af, const char *server_ip,
		const uint16_t server_port, const int timeout,
		const int flags, const char *bind_ipaddr, int *err_no)
{
    int sock;
    bool auto_detect;

    sock = socketCreateEx2(af, server_ip,
            flags, bind_ipaddr, err_no);
    if (sock < 0)
    {
        return sock;
    }

    auto_detect = ((flags & O_NONBLOCK) == 0);
    *err_no = connectserverbyip_nb_ex(sock, server_ip,
            server_port, timeout, auto_detect);
    if (*err_no != 0)
    {
        logError("file: "__FILE__", line: %d, "
                "connect to %s:%u fail, "
                "errno: %d, error info: %s",
                __LINE__, server_ip, server_port,
                *err_no, STRERROR(*err_no));
        close(sock);
        return -4;
    }

    return sock;
}

const char * fc_inet_ntop(const struct sockaddr *addr,
        char *buff, const int bufferSize)
{
    void *sin_addr;
    const char *output;

    if (addr->sa_family == AF_INET) {
        sin_addr = &((struct sockaddr_in *)addr)->sin_addr;
    } else if (addr->sa_family == AF_INET6) {
        sin_addr = &((struct sockaddr_in6 *)addr)->sin6_addr;
    } else {
        *buff = '\0';
        logWarning("file: "__FILE__", line: %d, "
                "unkown family: %d", __LINE__, addr->sa_family);
        return NULL;
    }

    if ((output=inet_ntop(addr->sa_family, sin_addr, buff, bufferSize)) == NULL)
    {
        *buff = '\0';
        logWarning("file: "__FILE__", line: %d, "
                "call inet_ntop fail, "
                "errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
    }

    return output;
}

in_addr_t getIpaddr(getnamefunc getname, int sock, \
		char *buff, const int bufferSize)
{
	struct sockaddr addr;
	socklen_t addrlen;

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);
	
	if (getname(sock, &addr, &addrlen) != 0)
	{
		*buff = '\0';
		return INADDR_NONE;
	}
	
	if (addrlen > 0)
	{
        fc_inet_ntop(&addr, buff, bufferSize);
	}
	else
	{
		*buff = '\0';
	}
	
	return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;  //DO NOT support IPv6
}

int getIpAndPort(getnamefunc getname, int sock,
		char *buff, const int bufferSize, int *port)
{
	struct sockaddr addr;
	socklen_t addrlen;

	memset(&addr, 0, sizeof(addr));
	addrlen = sizeof(addr);

	if (getname(sock, &addr, &addrlen) != 0)
	{
		*buff = '\0';
		return errno != 0 ? errno : EINVAL;
	}

	if (addrlen > 0)
	{
        fc_inet_ntop(&addr, buff, bufferSize);
	}
	else
	{
		*buff = '\0';
	}

    if (addr.sa_family == AF_INET) {
        *port = ntohs(((struct sockaddr_in *)&addr)->sin_port);
    } else {
        *port = ntohs(((struct sockaddr_in6 *)&addr)->sin6_port);
    }
	return 0;
}

char *getHostnameByIp(const char *szIpAddr, char *buff, const int bufferSize)
{
	struct hostent *ent;
    sockaddr_convert_t convert;

    if (setsockaddrbyip(szIpAddr, 0, &convert) != 0)
    {
		*buff = '\0';
		return buff;
    }

	ent = gethostbyaddr(&convert.sa.addr, convert.len,
            convert.sa.addr.sa_family);
	if (ent == NULL || ent->h_name == NULL)
	{
		*buff = '\0';
	}
	else
	{
		snprintf(buff, bufferSize, "%s", ent->h_name);
	}

	return buff;
}

in_addr_t getIpaddrByName(const char *name, char *buff, const int bufferSize)
{
	struct in_addr ip_addr;
	struct hostent *ent;
	in_addr_t **addr_list;

	if ((*name >= '0' && *name <= '9') && 
		inet_pton(AF_INET, name, &ip_addr) == 1)
	{
		if (buff != NULL)
		{
			snprintf(buff, bufferSize, "%s", name);
		}
		return ip_addr.s_addr;
	}

	ent = gethostbyname(name);
	if (ent == NULL)
	{
		return INADDR_NONE;
	}

    addr_list = (in_addr_t **)ent->h_addr_list;
	if (addr_list[0] == NULL)
	{
		return INADDR_NONE;
	}

	memset(&ip_addr, 0, sizeof(ip_addr));
	ip_addr.s_addr = *(addr_list[0]);
	if (buff != NULL)
	{
		if (inet_ntop(AF_INET, &ip_addr, buff, bufferSize) == NULL)
		{
			*buff = '\0';
		}
	}

	return ip_addr.s_addr;
}

int getIpaddrsByName(const char *name,
    ip_addr_t *ip_addr_arr, const int ip_addr_arr_size)
{
    int ip_count;
    struct sockaddr_in *addr;
    struct sockaddr_in6 *addr6;
    struct addrinfo hints, *res, *res0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(name, NULL, &hints, &res0) != 0) {
        return 0;
    }

    for (ip_count = 0, res = res0; res; res = res->ai_next) {
        if (res->ai_family != AF_INET6 && res->ai_family != AF_INET) {
            logError("file: "__FILE__", line: %d, " \
                     "unsupported family %d, " \
                     "only suppport AF_INET6 and AF_INET", \
                     __LINE__, res->ai_family);
            continue;
        }

        if (ip_addr_arr_size <= ip_count) {
            break;
        }

        if (res->ai_family == AF_INET6) {
            addr6 = (struct sockaddr_in6 *) res->ai_addr;
            if (inet_ntop(res->ai_family, &addr6->sin6_addr,
                    ip_addr_arr[ip_count].ip_addr, INET6_ADDRSTRLEN) == NULL)
            {
                logError("file: "__FILE__", line: %d, " \
                         "inet_ntop failed: %d, %s", \
                         __LINE__, errno, strerror(errno));
                continue;
            }
        } else {
            addr = (struct sockaddr_in *) res->ai_addr;
            if (inet_ntop(res->ai_family, &addr->sin_addr,
                    ip_addr_arr[ip_count].ip_addr, INET6_ADDRSTRLEN) == NULL)
            {
                logError("file: "__FILE__", line: %d, " \
                         "inet_ntop failed: %d, %s", \
                         __LINE__, errno, strerror(errno));
                continue;
            }
        }

        ip_addr_arr[ip_count++].socket_domain = res->ai_family;
    }

    freeaddrinfo(res0);

	return ip_count;
}

int nbaccept(int sock, const int timeout, int *err_no)
{
	struct sockaddr_in inaddr;
	socklen_t sockaddr_len;
	fd_set read_set;
	struct timeval t;
	int result;
	
	if (timeout > 0)
	{
		t.tv_usec = 0;
		t.tv_sec = timeout;
		
		FD_ZERO(&read_set);
		FD_SET(sock, &read_set);
		
		result = select(sock+1, &read_set, NULL, NULL, &t);
		if (result == 0)  //timeout
		{
			*err_no = ETIMEDOUT;
			return -1;
		}
		else if (result < 0) //error
		{
			*err_no = errno != 0 ? errno : EINTR;
			return -1;
		}
	
		/*
		if (!FD_ISSET(sock, &read_set))
		{
			*err_no = EAGAIN;
			return -1;
		}
		*/
	}
	
	sockaddr_len = sizeof(inaddr);
	result = accept(sock, (struct sockaddr*)&inaddr, &sockaddr_len);
	if (result < 0)
	{
		*err_no = errno != 0 ? errno : EINTR;
	}
	else
	{
		*err_no = 0;
	}

	return result;
}

int socketBind2(int af, int sock, const char *bind_ipaddr, const int port)
{
    sockaddr_convert_t convert;
    char bind_ip_prompt[256];
    int result;

    convert.sa.addr.sa_family = af;
	if (bind_ipaddr == NULL || *bind_ipaddr == '\0')
	{
        *bind_ip_prompt = '\0';
        if (af == AF_INET)
        {
            convert.len = sizeof(convert.sa.addr4);
            convert.sa.addr4.sin_port = htons(port);
		    convert.sa.addr4.sin_addr.s_addr = INADDR_ANY;
        }
        else
        {
            convert.len = sizeof(convert.sa.addr6);
            convert.sa.addr6.sin6_port = htons(port);
		    convert.sa.addr6.sin6_addr = in6addr_any;
        }
	}
	else
    {
        if ((result=setsockaddrbyip(bind_ipaddr, port, &convert)) != 0)
        {
            return result;
        }
        sprintf(bind_ip_prompt, "bind ip %s, ", bind_ipaddr);
    }

	if (bind(sock, &convert.sa.addr, convert.len) < 0)
	{
		logError("file: "__FILE__", line: %d, "
			"%sbind port %d failed, "
			"errno: %d, error info: %s.",
			__LINE__, bind_ip_prompt, port,
            errno, STRERROR(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	return 0;
}

int socketBind(int sock, const char *bind_ipaddr, const int port)
{
    return socketBind2(AF_INET, sock, bind_ipaddr, port);
}

int socketBindIPv6(int sock, const char *bind_ipaddr, const int port)
{
    return socketBind2(AF_INET6, sock, bind_ipaddr, port);
}

int socketServer2(int af, const char *bind_ipaddr, const int port, int *err_no)
{
	int sock;
	int result;
	//int socket(int domain, int type, int protocol);
    //AF_INET: ipv4
    //type: socket类型有，SOCK_STREAM(TCP)、SOCK_DGRAM、SOCK_RAW、SOCK_PACKET、SOCK_SEQPACKET
    //protocol协议, 有:IPPROTO_TCP、IPPTOTO_UDP、IPPROTO_SCTP、IPPROTO_TIPC等
	sock = socket(af, SOCK_STREAM, 0);
	if (sock < 0)
	{
        //EMFILE:已达到进程可同时打开的文件数上限
		*err_no = errno != 0 ? errno : EMFILE;
		logError("file: "__FILE__", line: %d, " \
			"socket create failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return -1;
	}

    FC_SET_CLOEXEC(sock);
    SET_SOCKOPT_NOSIGPIPE(sock);

	result = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &result, sizeof(int))<0)
	{
		*err_no = errno != 0 ? errno : ENOMEM;
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		close(sock);
		return -2;
	}

	if ((*err_no=socketBind2(af, sock, bind_ipaddr, port)) != 0)
	{
		close(sock);
		return -3;
	}
	//int listen(int sockfd, int backlog)
    //返回：0──成功， -1──失败
    //backlog 指定队列的容量, 这个队列用于记录正在连接但是还没有连接完成的客户端
	if (listen(sock, 1024) < 0)
	{
		*err_no = errno != 0 ? errno : EINVAL;
		logError("file: "__FILE__", line: %d, " \
			"listen port %d failed, " \
			"errno: %d, error info: %s", \
			__LINE__, port, errno, STRERROR(errno));
		close(sock);
		return -4;
	}

	*err_no = 0;
	return sock;
}

int socketServer(const char *bind_ipaddr, const int port, int *err_no)
{

    return socketServer2(AF_INET, bind_ipaddr, port, err_no);
}

int socketServerIPv6(const char *bind_ipaddr, const int port, int *err_no)
{
    return socketServer2(AF_INET6, bind_ipaddr, port, err_no);
}

int tcprecvfile(int sock, const char *filename, const int64_t file_bytes, \
		const int fsync_after_written_bytes, const int timeout, \
		int64_t *true_file_bytes)
{
	int write_fd;
	char buff[FAST_WRITE_BUFF_SIZE];
	int64_t remain_bytes;
	int recv_bytes;
	int written_bytes;
	int result;
	int flags;
	int count;
	tcprecvdata_exfunc recv_func;

	*true_file_bytes = 0;
	flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
	{
		return errno != 0 ? errno : EACCES;
	}

	if (flags & O_NONBLOCK)
	{
		recv_func = tcprecvdata_nb_ex;
	}
	else
	{
		recv_func = tcprecvdata_ex;
	}

	write_fd = open(filename, O_WRONLY | O_CREAT |
            O_TRUNC | O_CLOEXEC, 0644);
	if (write_fd < 0)
	{
		return errno != 0 ? errno : EACCES;
	}

	written_bytes = 0;
	remain_bytes = file_bytes;
	while (remain_bytes > 0)
	{
		if (remain_bytes > sizeof(buff))
		{
			recv_bytes = sizeof(buff);
		}
		else
		{
			recv_bytes = remain_bytes;
		}

		result = recv_func(sock, buff, recv_bytes,
				timeout, &count);
		if (result != 0)
		{
			if (file_bytes != INFINITE_FILE_SIZE)
			{
				close(write_fd);
				unlink(filename);
				return result;
			}
		}

		if (count > 0 && write(write_fd, buff, count) != count)
		{
			result = errno != 0 ? errno: EIO;
			close(write_fd);
			unlink(filename);
			return result;
		}

		*true_file_bytes += count;
		if (fsync_after_written_bytes > 0)
		{
			written_bytes += count;
			if (written_bytes >= fsync_after_written_bytes)
			{
				written_bytes = 0;
				if (fsync(write_fd) != 0)
				{
					result = errno != 0 ? errno: EIO;
					close(write_fd);
					unlink(filename);
					return result;
				}
			}
		}

		if (result != 0)  //recv infinite file, does not delete the file
		{
			int read_fd;
			read_fd = -1;

			do
			{
				if (*true_file_bytes < 8)
				{
					break;
				}

				read_fd = open(filename, O_RDONLY | O_CLOEXEC);
				if (read_fd < 0)
				{
					return errno != 0 ? errno : EACCES;
				}

				if (lseek(read_fd, -8, SEEK_END) < 0)
				{
					result = errno != 0 ? errno : EIO;
					break;
				}

				if (read(read_fd, buff, 8) != 8)
				{
					result = errno != 0 ? errno : EIO;
					break;
				}

				*true_file_bytes -= 8;
				if (buff2long(buff) != *true_file_bytes)
				{
					result = EINVAL;
					break;
				}

				if (ftruncate(write_fd, *true_file_bytes) != 0)
				{
					result = errno != 0 ? errno : EIO;
					break;
				}

				result = 0;
			} while (0);
		
			close(write_fd);
			if (read_fd >= 0)
			{
				close(read_fd);
			}

			if (result != 0)
			{
				unlink(filename);
			}

			return result;
		}

		remain_bytes -= count;
	}

	close(write_fd);
	return 0;
}

int tcprecvfile_ex(int sock, const char *filename, const int64_t file_bytes, \
		const int fsync_after_written_bytes, \
		unsigned int *hash_codes, const int timeout)
{
	int fd;
	char buff[FAST_WRITE_BUFF_SIZE];
	int64_t remain_bytes;
	int recv_bytes;
	int written_bytes;
	int result;
	int flags;
	tcprecvdata_exfunc recv_func;

	flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
	{
		return errno != 0 ? errno : EACCES;
	}

	if (flags & O_NONBLOCK)
	{
		recv_func = tcprecvdata_nb_ex;
	}
	else
	{
		recv_func = tcprecvdata_ex;
	}

	fd = open(filename, O_WRONLY | O_CREAT |
            O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
	{
		return errno != 0 ? errno : EACCES;
	}

	INIT_HASH_CODES4(hash_codes)
	
	written_bytes = 0;
	remain_bytes = file_bytes;
	while (remain_bytes > 0)
	{
		if (remain_bytes > sizeof(buff))
		{
			recv_bytes = sizeof(buff);
		}
		else
		{
			recv_bytes = remain_bytes;
		}

		if ((result=recv_func(sock, buff, recv_bytes, \
				timeout, NULL)) != 0)
		{
			close(fd);
			unlink(filename);
			return result;
		}

		if (write(fd, buff, recv_bytes) != recv_bytes)
		{
			result = errno != 0 ? errno: EIO;
			close(fd);
			unlink(filename);
			return result;
		}

		if (fsync_after_written_bytes > 0)
		{
			written_bytes += recv_bytes;
			if (written_bytes >= fsync_after_written_bytes)
			{
				written_bytes = 0;
				if (fsync(fd) != 0)
				{
					result = errno != 0 ? errno: EIO;
					close(fd);
					unlink(filename);
					return result;
				}
			}
		}

		CALC_HASH_CODES4(buff, recv_bytes, hash_codes)

		remain_bytes -= recv_bytes;
	}

	close(fd);

	FINISH_HASH_CODES4(hash_codes)

	return 0;
}

int tcpdiscard(int sock, const int bytes, const int timeout, \
		int64_t *total_recv_bytes)
{
	char buff[FAST_WRITE_BUFF_SIZE];
	int remain_bytes;
	int recv_bytes;
	int result;
	int flags;
	int count;
	tcprecvdata_exfunc recv_func;

	*total_recv_bytes = 0;
	flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
	{
		return errno != 0 ? errno : EACCES;
	}

	if (flags & O_NONBLOCK)
	{
		recv_func = tcprecvdata_nb_ex;
	}
	else
	{
		recv_func = tcprecvdata_ex;
	}
	
	remain_bytes = bytes;
	while (remain_bytes > 0)
	{
		if (remain_bytes > sizeof(buff))
		{
			recv_bytes = sizeof(buff);
		}
		else
		{
			recv_bytes = remain_bytes;
		}

		result = recv_func(sock, buff, recv_bytes, \
				timeout, &count);
		*total_recv_bytes += count;
		if (result != 0)
		{
			return result;
		}

		remain_bytes -= recv_bytes;
	}

	return 0;
}

int tcpsendfile_ex(int sock, const char *filename, const int64_t file_offset, \
	const int64_t file_bytes, const int timeout, int64_t *total_send_bytes)
{
	int fd;
	int64_t send_bytes;
	int result;
	int flags;
#ifdef USE_SENDFILE
   #if defined(OS_FREEBSD) || defined(OS_LINUX)
	off_t offset;
	int64_t remain_bytes;
   #endif
#else
	int64_t remain_bytes;
#endif

	fd = open(filename, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		*total_send_bytes = 0;
		return errno != 0 ? errno : EACCES;
	}

	flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
	{
		*total_send_bytes = 0;
		return errno != 0 ? errno : EACCES;
	}

#ifdef USE_SENDFILE

	if (flags & O_NONBLOCK)
	{
		if (fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0)
		{
			*total_send_bytes = 0;
			return errno != 0 ? errno : EACCES;
		}
	}

#ifdef OS_LINUX
	/*
	result = 1;
	if (setsockopt(sock, SOL_TCP, TCP_CORK, &result, sizeof(int)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s.", \
			__LINE__, errno, STRERROR(errno));
		close(fd);
		*total_send_bytes = 0;
		return errno != 0 ? errno : EIO;
	}
	*/

#define FILE_1G_SIZE    (1 * 1024 * 1024 * 1024)

	result = 0;
	offset = file_offset;
	remain_bytes = file_bytes;
	while (remain_bytes > 0)
	{
		if (remain_bytes > FILE_1G_SIZE)
		{
			send_bytes = sendfile(sock, fd, &offset, FILE_1G_SIZE);
		}
		else
		{
			send_bytes = sendfile(sock, fd, &offset, remain_bytes);
		}

		if (send_bytes <= 0)
		{
			result = errno != 0 ? errno : EIO;
            if (result == EINTR && try_again_when_interrupt)
            {
                continue;
            }
			break;
		}

		remain_bytes -= send_bytes;
	}

#else
#ifdef OS_FREEBSD
	offset = file_offset;
#if defined(DARWIN)
	result = 0;
	remain_bytes = file_bytes;
	while (remain_bytes > 0)
	{
        off_t len;
        len = remain_bytes;
        if (sendfile(fd, sock, offset, &len, NULL, 0) != 0) {
			result = errno != 0 ? errno : EIO;
            if (!(result == EINTR && try_again_when_interrupt))
            {
                break;
            }
        }
		remain_bytes -= len;
    }
#else
	remain_bytes = file_bytes;
    result = 0;
	while (remain_bytes > 0)
    {
        off_t sbytes;
        sbytes = 0;
        if (sendfile(fd, sock, offset, remain_bytes, NULL, &sbytes, 0) != 0)
        {
            result = errno != 0 ? errno : EIO;
            if (!(result == EINTR && try_again_when_interrupt))
            {
                break;
            }
        }
        remain_bytes -= sbytes;
    }
#endif
#endif

	*total_send_bytes = file_bytes - remain_bytes;
#endif

	if (flags & O_NONBLOCK)  //restore
	{
		if (fcntl(sock, F_SETFL, flags) < 0)
		{
			result = errno != 0 ? errno : EACCES;
		}
	}

#ifdef OS_LINUX
	close(fd);
	return result;
#endif

#ifdef OS_FREEBSD
	close(fd);
	return result;
#endif

#endif

	{
	char buff[FAST_WRITE_BUFF_SIZE];
	int64_t remain_bytes;
	tcpsenddatafunc send_func;

	if (file_offset > 0 && lseek(fd, file_offset, SEEK_SET) < 0)
	{
		result = errno != 0 ? errno : EIO;
		close(fd);
		*total_send_bytes = 0;
		return result;
	}

	if (flags & O_NONBLOCK)
	{
		send_func = tcpsenddata_nb;
	}
	else
	{
		send_func = tcpsenddata;
	}
	
	result = 0;
	remain_bytes = file_bytes;
	while (remain_bytes > 0)
	{
		if (remain_bytes > sizeof(buff))
		{
			send_bytes = sizeof(buff);
		}
		else
		{
			send_bytes = remain_bytes;
		}

		if (read(fd, buff, send_bytes) != send_bytes)
		{
			result = errno != 0 ? errno : EIO;
			break;
		}

		if ((result=send_func(sock, buff, send_bytes, \
				timeout)) != 0)
		{
			break;
		}

		remain_bytes -= send_bytes;
	}

	*total_send_bytes = file_bytes - remain_bytes;
	}

	close(fd);
	return result;
}

int tcpsetserveropt(int fd, const int timeout)
{
	struct linger linger;
	struct timeval waittime;

    SET_SOCKOPT_NOSIGPIPE(fd);

/*
	linger.l_onoff = 1;
#ifdef OS_FREEBSD
	linger.l_linger = timeout * 100;
#else
	linger.l_linger = timeout;
#endif
*/
    /**
     * linger.l_onoff = 0;
	linger.l_linger = 0;
    SO_DEBUG 打开或关闭排错模式
    SO_REUSEADDR 允许在bind ()过程中本地地址可重复使用
    SO_TYPE 返回socket 形态.
    SO_ERROR 返回socket 已发生的错误原因
    SO_DONTROUTE 送出的数据包不要利用路由设备来传输.
    SO_BROADCAST 使用广播方式传送
    SO_SNDBUF 设置送出的暂存区大小
    SO_RCVBUF 设置接收的暂存区大小
    SO_KEEPALIVE 定期确定连线是否已终止.
    SO_OOBINLINE 当接收到OOB 数据时会马上送至标准输入设备
    SO_LINGER 确保数据安全且可靠的传送出去.
    https://blog.csdn.net/zhangkuo8512067/article/details/43984243
     */
	if (setsockopt(fd, SOL_SOCKET, SO_LINGER, \
                &linger, (socklen_t)sizeof(struct linger)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	waittime.tv_sec = timeout;
	waittime.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
               &waittime, (socklen_t)sizeof(struct timeval)) < 0)
	{
		logWarning("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
	}

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               &waittime, (socklen_t)sizeof(struct timeval)) < 0)
	{
		logWarning("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
	}

	return tcpsetnodelay(fd, timeout);
}

int tcpsetkeepalive(int fd, const int idleSeconds)
{
	int keepAlive;

#ifdef OS_LINUX
	int keepIdle;
	int keepInterval;
	int keepCount;
#endif

	keepAlive = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, \
		(char *)&keepAlive, sizeof(keepAlive)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

#ifdef OS_LINUX
	keepIdle = idleSeconds;
	if (setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, (char *)&keepIdle, \
		sizeof(keepIdle)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

	keepInterval = 10;
	if (setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, (char *)&keepInterval, \
		sizeof(keepInterval)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

	keepCount = 3;
	if (setsockopt(fd, SOL_TCP, TCP_KEEPCNT, (char *)&keepCount, \
		sizeof(keepCount)) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}
#endif

	return 0;
}

int tcpprintkeepalive(int fd)
{
	int keepAlive;
	socklen_t len;

#ifdef OS_LINUX
	int keepIdle;
	int keepInterval;
	int keepCount;
#endif

	len = sizeof(keepAlive);
	if (getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, \
		(char *)&keepAlive, &len) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

#ifdef OS_LINUX
	len = sizeof(keepIdle);
	if (getsockopt(fd, SOL_TCP, TCP_KEEPIDLE, (char *)&keepIdle, \
		&len) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

	len = sizeof(keepInterval);
	if (getsockopt(fd, SOL_TCP, TCP_KEEPINTVL, (char *)&keepInterval, \
		&len) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

	len = sizeof(keepCount);
	if (getsockopt(fd, SOL_TCP, TCP_KEEPCNT, (char *)&keepCount, \
		&len) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"setsockopt failed, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}

	logInfo("keepAlive=%d, keepIdle=%d, keepInterval=%d, keepCount=%d", 
		keepAlive, keepIdle, keepInterval, keepCount);
#else
        logInfo("keepAlive=%d", keepAlive);
#endif

	return 0;
}

int tcpsetnonblockopt(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"fcntl failed, errno: %d, error info: %s.", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EACCES;
	}

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"fcntl failed, errno: %d, error info: %s.", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EACCES;
	}

	return 0;
}

int tcpsetnodelay(int fd, const int timeout)
{
	int flags;
	int result;

	if ((result=tcpsetkeepalive(fd, 2 * timeout + 1)) != 0)
	{
		return result;
	}

	flags = 1;
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			(char *)&flags, sizeof(flags)) < 0)
	{
		logError("file: "__FILE__", line: %d, "
			"setsockopt failed, errno: %d, error info: %s",
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EINVAL;
	}
    TCP_SET_QUICK_ACK(fd);

	return 0;
}

#if defined(OS_LINUX) || defined(OS_FREEBSD)
int getlocaladdrs(char ip_addrs[][IP_ADDRESS_SIZE], \
	const int max_count, int *count)
{
	struct ifaddrs *ifc;
	struct ifaddrs *ifc1;

	*count = 0;
	if (0 != getifaddrs(&ifc))
	{
		logError("file: "__FILE__", line: %d, " \
			"call getifaddrs fail, " \
			"errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EMFILE;
	}

	ifc1 = ifc;
	while (NULL != ifc)
    {
        struct sockaddr *s;
        s = ifc->ifa_addr;
        if (NULL != s && AF_INET == s->sa_family)
        {
            if (max_count <= *count)
            {
                logError("file: "__FILE__", line: %d, "\
                        "max_count: %d < iterface count: %d", \
                        __LINE__, max_count, *count);
                freeifaddrs(ifc1);
                return ENOSPC;
            }

            if (inet_ntop(AF_INET, &((struct sockaddr_in *)s)-> \
                        sin_addr, ip_addrs[*count], IP_ADDRESS_SIZE) != NULL)
            {
                (*count)++;
            }
            else
            {
                logWarning("file: "__FILE__", line: %d, " \
                        "call inet_ntop fail, " \
                        "errno: %d, error info: %s", \
                        __LINE__, errno, STRERROR(errno));
            }
        }

        ifc = ifc->ifa_next;
    }

	freeifaddrs(ifc1);
	return *count > 0 ? 0 : ENOENT;
}

#else

int getlocaladdrs(char ip_addrs[][IP_ADDRESS_SIZE], \
	const int max_count, int *count)
{
	int s;
	struct ifconf ifconf;
	struct ifreq ifr[32];
	struct ifreq *ifrp;
	char *p_end;
	int result;

	*count = 0;
	s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"socket create fail, errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EMFILE;
	}

	ifconf.ifc_buf = (char *) ifr;
	ifconf.ifc_len = sizeof(ifr);
	if (ioctl(s, SIOCGIFCONF, &ifconf) < 0)
	{
		result = errno != 0 ? errno : EMFILE;
		logError("file: "__FILE__", line: %d, " \
			"call ioctl fail, errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
 		close(s);
		return result;
	}

	ifrp = ifconf.ifc_req;
	p_end = (char *)ifr + ifconf.ifc_len;
	while ((char *)ifrp < p_end)
	{
		struct sockaddr *sa = &ifrp->ifr_addr;
		struct sockaddr_in *s_in;

		if (*count >= max_count)
		{
			logError("file: "__FILE__", line: %d, " \
					"max_count: %d < iterface count: %d", \
					__LINE__, max_count, *count);
			close(s);
			return ENOSPC;
		}

        s_in = (struct sockaddr_in *) &ifrp->ifr_addr;
		if (sa->sa_family == AF_INET)
        {
            if (!inet_ntop(AF_INET, &s_in->sin_addr, \
                        ip_addrs[*count], IP_ADDRESS_SIZE))
            {
                result = errno != 0 ? errno : EMFILE;
                logError("file: "__FILE__", line: %d, " \
                        "call inet_ntop fail, " \
                        "errno: %d, error info: %s", \
                        __LINE__, result, STRERROR(result));
                close(s);
                return result;
            }
            (*count)++;
        }

#ifdef OS_FREEBSD
		ifrp = (struct ifreq*)((caddr_t)&ifrp->ifr_addr + sa->sa_len);
#else
        ifrp++;
#endif
	}

	close(s);
	return *count > 0 ? 0 : ENOENT;
}

#endif

int gethostaddrs(char **if_alias_prefixes, const int prefix_count, \
	char ip_addrs[][IP_ADDRESS_SIZE], const int max_count, int *count)
{
	struct hostent *ent;
	char hostname[128];
	char *alias_prefixes1[1];
	char **true_alias_prefixes;
	int true_count;
	int i;
	int k;
	int sock;
	struct ifreq req;
	struct sockaddr_in *addr;
	int ret;

	*count = 0;
	if (prefix_count <= 0)
	{
		if (getlocaladdrs(ip_addrs, max_count, count) == 0)
		{
			return 0;
		}

#ifdef OS_FREEBSD
	#define IF_NAME_PREFIX    "bge"
#else
  #ifdef OS_SUNOS
	#define IF_NAME_PREFIX   "e1000g"
  #else
      #ifdef OS_AIX
          #define IF_NAME_PREFIX   "en"
      #else
          #define IF_NAME_PREFIX   "eth"
      #endif
  #endif
#endif

  		alias_prefixes1[0] = IF_NAME_PREFIX;
		true_count = 1;
		true_alias_prefixes = alias_prefixes1;
	}
	else
	{
		true_count = prefix_count;
		true_alias_prefixes = if_alias_prefixes;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"socket create failed, errno: %d, error info: %s.", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EMFILE;
	}

	for (i=0; i<true_count && *count<max_count; i++)
	{
	for (k=0; k<max_count; k++)
	{
		memset(&req, 0, sizeof(req));
		sprintf(req.ifr_name, "%s%d", true_alias_prefixes[i], k);
		ret = ioctl(sock, SIOCGIFADDR, &req);
		if (ret == -1)
		{
            if (*count == 0 && k == 0)  //maybe based 1
            {
                continue;
            }
			break;
		}

		addr = (struct sockaddr_in*)&req.ifr_addr;
		if (inet_ntop(AF_INET, &addr->sin_addr, ip_addrs[*count], \
			IP_ADDRESS_SIZE) != NULL)
		{
			(*count)++;
			if (*count >= max_count)
			{
				break;
			}
		}
	}
	}

	close(sock);
	if (*count > 0)
	{
		return 0;
	}

	if (gethostname(hostname, sizeof(hostname)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call gethostname fail, " \
			"error no: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EFAULT;
	}

        ent = gethostbyname(hostname);
	if (ent == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"call gethostbyname fail, " \
			"error no: %d, error info: %s", \
			__LINE__, h_errno, STRERROR(h_errno));
		return h_errno != 0 ? h_errno : EFAULT;
	}

	k = 0;
	while (ent->h_addr_list[k] != NULL)
	{
		if (*count >= max_count)
		{
			break;
		}

		if (inet_ntop(ent->h_addrtype, ent->h_addr_list[k], \
			ip_addrs[*count], IP_ADDRESS_SIZE) != NULL)
		{
			(*count)++;
		}

		k++;
	}

	return 0;
}

#if defined(OS_LINUX) || defined(OS_FREEBSD)

static inline void formatifmac(char *buff, const int buff_size, unsigned char *hwaddr)
{
    int i;
    for (i=0; i<6; i++)
    {
        if (hwaddr[i] != 0)
        {
            break;
        }
    }

    if (i == 6)
    {
        *buff = '\0';
        return;
    }

    snprintf(buff, buff_size,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            *hwaddr, *(hwaddr+1), *(hwaddr+2),
            *(hwaddr+3), *(hwaddr+4), *(hwaddr+5));
}

#if defined(OS_LINUX)
static int getifmac(FastIFConfig *config)
{
    int sockfd;
    struct ifreq req[1];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        logError("file: "__FILE__", line: %d, "
                "unable to create socket, "
                "errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    memset(req, 0, sizeof(struct ifreq));
    strcpy(req->ifr_name, config->name);
    if (ioctl(sockfd, SIOCGIFHWADDR, req) < 0)
    {
        logError("file: "__FILE__", line: %d, "
                "ioctl error, errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
        close(sockfd);
        return errno != 0 ? errno : EPERM;
    }

    close(sockfd);
    formatifmac(config->mac, sizeof(config->mac),
            (unsigned char *)req->ifr_hwaddr.sa_data);
    return 0;
}
#else  //FreeBSD
static int getifmac(FastIFConfig *config)
{
    int                 mib[6];
    size_t              len;
    char                buf[256];
    unsigned char       *ptr;
    struct if_msghdr    *ifm;
    struct sockaddr_dl  *sdl;

    mib[0] = CTL_NET;
    mib[1] = AF_ROUTE;
    mib[2] = 0;
    mib[3] = AF_LINK;
    mib[4] = NET_RT_IFLIST;

    if ((mib[5] = if_nametoindex(config->name)) == 0)
    {
        logError("file: "__FILE__", line: %d, "
                "call if_nametoindex fail, "
                "errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    len = sizeof(buf);
    if (sysctl(mib, 6, buf, &len, NULL, 0) < 0)
    {
        logError("file: "__FILE__", line: %d, "
                "call sysctl fail, "
                "errno: %d, error info: %s",
                __LINE__, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    ifm = (struct if_msghdr *)buf;
    sdl = (struct sockaddr_dl *)(ifm + 1);
    ptr = (unsigned char *)LLADDR(sdl);
    formatifmac(config->mac, sizeof(config->mac), ptr);
    return 0;
}
#endif

int getifconfigs(FastIFConfig *if_configs, const int max_count, int *count)
{
	struct ifaddrs *ifc;
	struct ifaddrs *ifc1;
    FastIFConfig *config;
    char *buff;
    void *sin_addr;
    int buff_size;
    int i;

	*count = 0;
    memset(if_configs, 0, sizeof(FastIFConfig) * max_count);
	if (0 != getifaddrs(&ifc))
	{
		logError("file: "__FILE__", line: %d, " \
			"call getifaddrs fail, " \
			"errno: %d, error info: %s", \
			__LINE__, errno, STRERROR(errno));
		return errno != 0 ? errno : EMFILE;
	}

	ifc1 = ifc;
	while (NULL != ifc)
	{
		struct sockaddr *s;
		s = ifc->ifa_addr;
		if (NULL != s)
        {
            if (AF_INET == s->sa_family || AF_INET6 == s->sa_family)
            {
                for (i=0; i<*count; i++)
                {
                    if (strcmp(if_configs[i].name, ifc->ifa_name) == 0)
                    {
                        break;
                    }
                }

                config = if_configs + i;
                if (i == *count)  //not found
                {
                    if (max_count <= *count)
                    {
                        logError("file: "__FILE__", line: %d, "\
                                "max_count: %d < iterface count: %d", \
                                __LINE__, max_count, *count);
                        freeifaddrs(ifc1);
                        return ENOSPC;
                    }

                    sprintf(config->name, "%s", ifc->ifa_name);
                    (*count)++;
                }

                if (AF_INET == s->sa_family)
                {
                    buff = config->ipv4;
                    buff_size = sizeof(config->ipv4);
                    sin_addr = &((struct sockaddr_in *)s)->sin_addr;
                }
                else
                {
                    buff = config->ipv6;
                    buff_size = sizeof(config->ipv6);
                    sin_addr = &((struct sockaddr_in6 *)s)->sin6_addr;
                }

                if (inet_ntop(s->sa_family, sin_addr, buff, buff_size) == NULL)
                {
                    logWarning("file: "__FILE__", line: %d, " \
                            "call inet_ntop fail, " \
                            "errno: %d, error info: %s", \
                            __LINE__, errno, STRERROR(errno));
                }
            }
        }

		ifc = ifc->ifa_next;
	}

	freeifaddrs(ifc1);

    for (i=0; i<*count; i++)
    {
        getifmac(if_configs + i);
    }

    return 0;
}

#else

int getifconfigs(FastIFConfig *if_configs, const int max_count, int *count)
{
    *count = 0;
    return EOPNOTSUPP;
}
#endif

int fc_get_net_type_by_ip(const char *ip)
{
    int len;
    if (ip == NULL)
    {
        return FC_NET_TYPE_NONE;
    }
    len = strlen(ip);
    if (len < 8)
    {
        return (len < 7) ? FC_NET_TYPE_NONE : FC_NET_TYPE_OUTER;
    }

    if (memcmp(ip, "10.", 3) == 0)
    {
        return FC_SUB_NET_TYPE_INNER_10;
    }

    if (memcmp(ip, "192.168.", 8) == 0)
    {
        return FC_SUB_NET_TYPE_INNER_192;
    }

    if (memcmp(ip, "172.", 4) == 0)
    {
        int b;
        b = atoi(ip + 4);
        if (b >= 16 && b < 32)
        {
            return FC_SUB_NET_TYPE_INNER_172;
        }
    }

    return FC_NET_TYPE_OUTER;
}

int fc_get_net_type_by_name(const char *net_type)
{
    if (net_type == NULL || *net_type == '\0') {
        return FC_NET_TYPE_ANY;
    }

    if (strcasecmp(net_type, NET_TYPE_ANY_STR) == 0) {
        return FC_NET_TYPE_ANY;
    } else if (strcasecmp(net_type, NET_TYPE_OUTER_STR) == 0) {
        return FC_NET_TYPE_OUTER;
    } else if (strcasecmp(net_type, NET_TYPE_INNER_STR) == 0) {
        return FC_NET_TYPE_INNER;
    } else if (strcasecmp(net_type, SUB_NET_TYPE_INNER_10_STR) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_10_STR2) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_10_STR3) == 0)
    {
        return FC_SUB_NET_TYPE_INNER_10;
    } else if (strcasecmp(net_type, SUB_NET_TYPE_INNER_172_STR) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_172_STR2) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_172_STR3) == 0)
    {
        return FC_SUB_NET_TYPE_INNER_172;
    } else if (strcasecmp(net_type, SUB_NET_TYPE_INNER_192_STR) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_192_STR2) == 0 ||
            strcasecmp(net_type, SUB_NET_TYPE_INNER_192_STR3) == 0)
    {
        return FC_SUB_NET_TYPE_INNER_192;
    } else {
        return FC_NET_TYPE_NONE;
    }
}

