/**
 * @file mavconn_tcp.cpp
 * @author Vladimir Ermakov <vooon341@gmail.com>
 *
 * @addtogroup mavconn
 * @{
 */
/*
 * Copyright 2014 Vladimir Ermakov.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <mavros/utils.h>
#include <mavros/mavconn_tcp.h>
#include <ros/console.h>
#include <ros/assert.h>

namespace mavconn {
using boost::system::error_code;
using boost::asio::io_service;
using boost::asio::ip::tcp;
using boost::asio::buffer;
typedef std::lock_guard<std::recursive_mutex> lock_guard;


static bool resolve_address_tcp(io_service &io, std::string host, unsigned short port, tcp::endpoint &ep)
{
	bool result = false;
	tcp::resolver resolver(io);
	error_code ec;

	tcp::resolver::query query(host, "");
	std::for_each(resolver.resolve(query, ec), tcp::resolver::iterator(),
		[&](const tcp::endpoint &q_ep) {
			ep = q_ep;
			ep.port(port);
			result = true;
			ROS_DEBUG_STREAM_NAMED("mavconn", "tcp: host " << host << " resolved as " << ep);
		});

	if (ec) {
		ROS_WARN_STREAM_NAMED("mavconn", "tcp: resolve error: " << ec);
		result = false;
	}

	return result;
}


/* -*- TCP client variant -*- */

MAVConnTCPClient::MAVConnTCPClient(uint8_t system_id, uint8_t component_id,
		std::string server_host, unsigned short server_port) :
	MAVConnInterface(system_id, component_id),
	tx_in_progress(false),
	io_service(),
	io_work(new io_service::work(io_service)),
	socket(io_service)
{
	if (!resolve_address_tcp(io_service, server_host, server_port, server_ep))
		throw DeviceError("tcp: resolve", "Bind address resolve failed");

	ROS_INFO_STREAM_NAMED("mavconn", "tcp" << channel << ": Server address: " << server_ep);

	try {
		socket.open(tcp::v4());
		socket.connect(server_ep);
	}
	catch (boost::system::system_error &err) {
		throw DeviceError("tcp", err);
	}

	// give some work to io_service before start
	io_service.post(boost::bind(&MAVConnTCPClient::do_recv, this));

	// run io_service for async io
	std::thread t(boost::bind(&io_service::run, &this->io_service));
	mavutils::set_thread_name(t, "MAVConnTCPc%d", channel);
	io_thread.swap(t);
}

MAVConnTCPClient::MAVConnTCPClient(uint8_t system_id, uint8_t component_id,
		boost::asio::io_service &server_io, int server_channel,
		tcp::socket &client_sock, tcp::endpoint &client_ep) :
	MAVConnInterface(system_id, component_id),
	socket(std::move(client_sock)),
	server_ep(std::move(client_ep))
{
	ROS_INFO_STREAM_NAMED("mavconn", "tcp-l" << server_channel <<
			": Got client, channel: " << channel <<
			", address: " << server_ep);

	// start recv on server io service
	server_io.post(boost::bind(&MAVConnTCPClient::do_recv, this));
}

MAVConnTCPClient::~MAVConnTCPClient() {
	close();
}

void MAVConnTCPClient::close() {
	lock_guard lock(mutex);
	if (!socket.is_open())
		return;

	io_work.reset();
	io_service.stop();
	socket.close();

	// clear tx queue
	std::for_each(tx_q.begin(), tx_q.end(),
			[](MsgBuffer *p) { delete p; });
	tx_q.clear();

	/* emit */ port_closed();
	if (io_thread.joinable())
		io_thread.join();
}

void MAVConnTCPClient::send_bytes(const uint8_t *bytes, size_t length)
{
	ROS_ASSERT_MSG(is_open(), "Should not send messages on closed channel!");
	MsgBuffer *buf = new MsgBuffer(bytes, length);
	{
		lock_guard lock(mutex);
		tx_q.push_back(buf);
		io_service.post(boost::bind(&MAVConnTCPClient::do_send, this, true));
	}
}

void MAVConnTCPClient::send_message(const mavlink_message_t *message, uint8_t sysid, uint8_t compid)
{
	ROS_ASSERT_MSG(is_open(), "Should not send messages on closed channel!");
	ROS_ASSERT(message != nullptr);
	MsgBuffer *buf = nullptr;

	/* if sysid/compid pair not match we need explicit finalize
	 * else just copy to buffer */
	if (message->sysid != sysid || message->compid != compid) {
		mavlink_message_t msg = *message;

#if MAVLINK_CRC_EXTRA
		mavlink_finalize_message_chan(&msg, sysid, compid, channel, message->len, mavlink_crcs[msg.msgid]);
#else
		mavlink_finalize_message_chan(&msg, sysid, compid, channel, message->len);
#endif

		buf = new MsgBuffer(&msg);
	}
	else
		buf = new MsgBuffer(message);

	ROS_DEBUG_NAMED("mavconn", "tcp%d:send: Message-ID: %d [%d bytes] Sys-Id: %d Comp-Id: %d",
			channel, message->msgid, message->len, sysid, compid);

	{
		lock_guard lock(mutex);
		tx_q.push_back(buf);
		io_service.post(boost::bind(&MAVConnTCPClient::do_send, this, true));
	}
}

void MAVConnTCPClient::do_recv()
{
	socket.async_receive(
			buffer(rx_buf, sizeof(rx_buf)),
			boost::bind(&MAVConnTCPClient::async_receive_end,
				this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
}

void MAVConnTCPClient::async_receive_end(error_code error, size_t bytes_transferred)
{
	mavlink_message_t message;
	mavlink_status_t status;

	if (error) {
		ROS_ERROR_STREAM_NAMED("mavconn", "tcp" << channel << ":receive: " << error);
		close();
		return;
	}

	for (ssize_t i = 0; i < bytes_transferred; i++) {
		if (mavlink_parse_char(channel, rx_buf[i], &message, &status)) {
			ROS_DEBUG_NAMED("mavconn", "tcp%d:recv: Message-Id: %d [%d bytes] Sys-Id: %d Comp-Id: %d",
					channel, message.msgid, message.len, message.sysid, message.compid);

			/* emit */ message_received(&message, message.sysid, message.compid);
		}
	}

	do_recv();
}

void MAVConnTCPClient::do_send(bool check_tx_state)
{
	if (check_tx_state && tx_in_progress)
		return;

	lock_guard lock(mutex);
	if (tx_q.empty())
		return;

	MsgBuffer *buf = tx_q.front();
	socket.async_send(
			buffer(buf->dpos(), buf->nbytes()),
			boost::bind(&MAVConnTCPClient::async_send_end,
				this,
				boost::asio::placeholders::error,
				boost::asio::placeholders::bytes_transferred));
}

void MAVConnTCPClient::async_send_end(error_code error, size_t bytes_transferred)
{
	if (error) {
		ROS_ERROR_STREAM_NAMED("mavconn", "tcp" << channel << ":sendto: " << error);
		close();
		return;
	}

	lock_guard lock(mutex);
	if (tx_q.empty())
		return;

	MsgBuffer *buf = tx_q.front();
	buf->pos += bytes_transferred;
	if (buf->nbytes() == 0) {
		tx_q.pop_front();
		delete buf;
	}

	if (!tx_q.empty())
		do_send(false);
}


/* -*- TCP server variant -*- */

MAVConnTCPServer::MAVConnTCPServer(uint8_t system_id, uint8_t component_id,
		std::string server_host, unsigned short server_port) :
	MAVConnInterface(system_id, component_id),
	io_service(),
	//io_work(new io_service::work(io_service)),
	acceptor(io_service),
	client_sock(io_service)
{
	if (!resolve_address_tcp(io_service, server_host, server_port, bind_ep))
		throw DeviceError("tcp-l: resolve", "Bind address resolve failed");

	ROS_INFO_STREAM_NAMED("mavconn", "tcp-l" << channel <<
			": Bind address: " << bind_ep);

	try {
		acceptor.open(tcp::v4());
		acceptor.set_option(tcp::acceptor::reuse_address(true));
		acceptor.bind(bind_ep);
		acceptor.listen(channes_available());
	}
	catch (boost::system::system_error &err) {
		throw DeviceError("tcp-l", err);
	}

	// give some work to io_service before start
	io_service.post(boost::bind(&MAVConnTCPServer::do_accept, this));

	// run io_service for async io
	std::thread t(boost::bind(&io_service::run, &this->io_service));
	mavutils::set_thread_name(t, "MAVConnTCPs%d", channel);
	io_thread.swap(t);
}

MAVConnTCPServer::~MAVConnTCPServer() {
	close();
}

void MAVConnTCPServer::close() {
	lock_guard lock(mutex);
	if (!acceptor.is_open())
		return;

	ROS_INFO_NAMED("mavconn", "tcp-l%d: Terminating server. "
			"All connections will be closed.", channel);

	if (!client_list.empty()) {
		std::for_each(client_list.begin(), client_list.end(),
				[&](MAVConnTCPClient *instp) {
				ROS_DEBUG_STREAM_NAMED("mavconn", "tcp-l" << channel <<
					": Close client " << instp->server_ep <<
					", channel " << instp->channel);
				instp->port_closed.disconnect_all_slots();
				instp->close();
				delete instp;
				});
		client_list.clear();
	}

	//io_work.reset();
	io_service.stop();
	acceptor.close();
	/* emit */ port_closed();

	if (io_thread.joinable())
		io_thread.join();
}

void MAVConnTCPServer::send_bytes(const uint8_t *bytes, size_t length)
{
	lock_guard lock(mutex);
	std::for_each(client_list.begin(), client_list.end(),
			[&](MAVConnTCPClient *instp) {
		instp->send_bytes(bytes, length);
	});
}

void MAVConnTCPServer::send_message(const mavlink_message_t *message, uint8_t sysid, uint8_t compid)
{
	lock_guard lock(mutex);
	std::for_each(client_list.begin(), client_list.end(),
			[&](MAVConnTCPClient *instp) {
		instp->send_message(message, sysid, compid);
	});
}

void MAVConnTCPServer::do_accept()
{
	acceptor.async_accept(
			client_sock,
			client_ep,
			boost::bind(&MAVConnTCPServer::async_accept_end,
				this,
				boost::asio::placeholders::error));
}

void MAVConnTCPServer::async_accept_end(error_code error)
{
	if (error) {
		ROS_ERROR_STREAM_NAMED("mavconn", "tcp-l" << channel << ":accept error: " << error);
		close();
		return;
	}

	if (channes_available() <= 0) {
		ROS_ERROR_NAMED("mavconn", "tcp-l:accept_cb: all channels in use, drop connection");
		client_sock.close();
		return;
	}

	MAVConnTCPClient *instp = new MAVConnTCPClient(sys_id, comp_id, io_service, channel, client_sock, client_ep);
	instp->message_received.connect(boost::bind(&MAVConnTCPServer::recv_message, this, _1, _2, _3));
	instp->port_closed.connect(boost::bind(&MAVConnTCPServer::client_closed, this, instp));

	client_list.push_back(instp);
	do_accept();
}

void MAVConnTCPServer::client_closed(MAVConnTCPClient *instp)
{
	ROS_INFO_STREAM_NAMED("mavconn", "tcp-l" << channel <<
			": Client connection closed, channel: " << instp->channel <<
			", address: " << instp->server_ep);

	client_list.remove(instp);
	delete instp;
}

void MAVConnTCPServer::recv_message(const mavlink_message_t *message, uint8_t sysid, uint8_t compid)
{
	message_received(message, sysid, compid);
}

}; // namespace mavconn
