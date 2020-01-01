#include <array>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <sstream>
#include <string>
#include <fstream>
#include <cstring>

using namespace std;
using namespace boost::asio;

io_service global_io_service;

class SOCKS4Request
{
private:
	unsigned char _vn;
	unsigned char _cd;
	unsigned char _dstport[2];
	ip::address_v4::bytes_type _dstip;
	unsigned char _null;
	boost::array<mutable_buffer, 6> _buffs;

public:
	SOCKS4Request()
	{
		_buffs = {
			buffer(&_vn, 1),
			buffer(&_cd, 1),
			buffer(_dstport),
			buffer(_dstip),
			buffer(&_null, 1)};
	}

	boost::array<mutable_buffer, 6> getbuffs()
	{
		return _buffs;
	}

	void setbuffs(string buf)
	{
		_vn = buf[0];
		_cd = buf[1];
		_dstport[0] = buf[2];
		_dstport[1] = buf[3];
		_dstip[0] = buf[4];
		_dstip[1] = buf[5];
		_dstip[2] = buf[6];
		_dstip[3] = buf[7];
		_null = 0x00;
	}

	char getcd()
	{
		return _cd;
	}

	char getvn()
	{
		return _vn;
	}

	string getdstport()
	{
		return to_string(((_dstport[0] << 8) & 0xff00) | _dstport[1]);
	}

	string getdstip()
	{
		ip::address_v4 addr(_dstip);
		return addr.to_string();
	}
};

class SOCKS4Reply
{
private:
	unsigned char _vn;
	unsigned char _cd;
	unsigned char _dstport[2];
	ip::address_v4::bytes_type _dstip;
	boost::array<mutable_buffer, 4> _buffs;

public:
	SOCKS4Reply(string dstip, string dstport, unsigned char cd) : _vn(0x00), _cd(cd)
	{
		_dstport[0] = (stoi(dstport) >> 8) & 0xff;
		_dstport[1] = stoi(dstport) & 0xff;
		_dstip = ip::address_v4::from_string(dstip).to_bytes();
	}

	boost::array<mutable_buffer, 4> getbuffs()
	{
		_buffs = {buffer(&_vn, 1),
				  buffer(&_cd, 1),
				  buffer(_dstport),
				  buffer(_dstip)};
		return _buffs;
	}
};

class SOCKS4Session : public enable_shared_from_this<SOCKS4Session>
{
private:
	enum
	{
		max_length = 4096
	};
	ip::tcp::acceptor bind_acceptor{global_io_service};
	ip::tcp::resolver resolv{global_io_service};
	ip::tcp::socket tcp_socket{global_io_service};
	ip::tcp::socket _socket;
	SOCKS4Request SOCKS4req;
	array<char, max_length> _data1;
	array<char, max_length> _data2;
	array<char, max_length> _socks4_reqbuf;

public:
	SOCKS4Session(ip::tcp::socket socket) : _socket(move(socket)) {}

	void start() { recv_socks_req(); }

private:
	void recv_socks_req()
	{
		auto self(shared_from_this());
		_socket.async_read_some(
			buffer(_socks4_reqbuf),
			[this, self](boost::system::error_code ec, size_t length) {
				string buf(_socks4_reqbuf.data(), length);
				SOCKS4req.setbuffs(buf);
				char vn = SOCKS4req.getvn();
				if (vn != 0x04)
					_socket.close();
				else
				{
					string dstip = SOCKS4req.getdstip();
					string dstport = SOCKS4req.getdstport();
					string srcip = _socket.remote_endpoint().address().to_string();
					string srcport = to_string(_socket.remote_endpoint().port());
					char cd = SOCKS4req.getcd();
					if (!firewall(cd, dstip))
					{
						string cmd = (cd == 0x01) ? "CONNECT" : "BIND";
						string reply = "Reject";
						print_server_msg(srcip, srcport, dstip, dstport, cmd, reply);
						SOCKS4Reply s4reply(dstip, dstport, 0x5b);
						send_socks4_reply(s4reply, 3);
					}
					else
					{
						if (cd == 0x01)
						{
							string cmd = "CONNECT";
							string reply = "Accept";
							print_server_msg(srcip, srcport, dstip, dstport, cmd, reply);
							ip::tcp::resolver::query q(dstip, dstport);
							do_resolve(dstip, dstport, q);
						}
						else if (cd == 0x02)
						{
							string cmd = "BIND";
							string reply = "Accept";
							print_server_msg(srcip, srcport, dstip, dstport, cmd, reply);
							unsigned short port(0);
							ip::tcp::endpoint bind_endpoint(ip::tcp::endpoint(ip::tcp::v4(), port));
							bind_acceptor.open(bind_endpoint.protocol());
							bind_acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
							bind_acceptor.bind(bind_endpoint);
							bind_acceptor.listen();

							string bind_ip = bind_acceptor.local_endpoint().address().to_string();
							string bind_port = to_string(bind_acceptor.local_endpoint().port());
							SOCKS4Reply s4reply(bind_ip, bind_port, 0x5a);
							send_socks4_reply(s4reply, 2);
						}
						else
						{
							_socket.close();
						}
					}
				}
			});
	}

	void do_resolve(string dstip, string dstport, ip::tcp::resolver::query q)
	{
		auto self(shared_from_this());
		resolv.async_resolve(q,
							 [this, self, dstip, dstport](boost::system::error_code ec, ip::tcp::resolver::iterator it) {
								 if (!ec)
									 do_connect(it, dstip, dstport);
								 else
								 {
									 _socket.close();
								 }
							 });
	}

	void do_connect(ip::tcp::resolver::iterator it, string dstip, string dstport)
	{
		auto self(shared_from_this());
		tcp_socket.async_connect(*it, [this, self, dstip, dstport](boost::system::error_code ec) {
			if (!ec)
			{
				SOCKS4Reply reply(dstip, dstport, 0x5a);
				send_socks4_reply(reply, 1);
			}
			else
			{
				_socket.close();
				tcp_socket.close();
			}
		});
	}

	void do_accept(SOCKS4Reply reply)
	{
		auto self(shared_from_this());
		bind_acceptor.async_accept(tcp_socket, [this, self, reply](boost::system::error_code ec) {
			if (!ec)
			{
				send_socks4_reply(reply, 1);
			}
			else
			{
				_socket.close();
				tcp_socket.close();
			}
		});
	}

	void send_socks4_reply(SOCKS4Reply reply, int act)
	{
		auto self(shared_from_this());
		_socket.async_send(
			reply.getbuffs(),
			[this, self, act, reply](boost::system::error_code ec, size_t /* length */) {
				if (!ec)
				{
					if (act == 1)
						relay();
					else if (act == 2)
						do_accept(reply);
					else
						_socket.close();
				}
				else
				{
					_socket.close();
					if (act == 1)
						tcp_socket.close();
				}
			});
	}

	void relay()
	{
		host_relay();
		client_relay();
	}

	void client_relay()
	{
		auto self(shared_from_this());
		_socket.async_read_some(
			buffer(_data1, max_length),
			[this, self](boost::system::error_code ec, size_t length) {
				if (!ec)
				{
					string data(_data1.data(), length);
					send_client_relay(data, length);
				}
				else
				{
					_socket.close();
					tcp_socket.close();
				}
			});
	}

	void send_client_relay(string data, size_t len)
	{
		auto self(shared_from_this());
		async_write(tcp_socket,
					buffer(_data1, len),
					[this, self, data](boost::system::error_code ec, size_t length) {
						if (!ec)
						{
							client_relay();
						}
						else
						{
							_socket.close();
							tcp_socket.close();
						}
					});
	}

	void host_relay()
	{
		auto self(shared_from_this());
		tcp_socket.async_read_some(
			buffer(_data2, max_length),
			[this, self](boost::system::error_code ec, size_t length) {
				if (!ec)
				{
					string data(_data2.data(), length);
					send_host_relay(data, length);
				}
				else
				{
					_socket.close();
					tcp_socket.close();
				}
			});
	}

	void send_host_relay(string data, size_t len)
	{
		auto self(shared_from_this());
		async_write(_socket,
					buffer(_data2, len),
					[this, self, data](boost::system::error_code ec, size_t length) {
						if (!ec)
						{
							host_relay();
						}
						else
						{
							_socket.close();
							tcp_socket.close();
						}
					});
	}

	bool firewall(char cd, string ip)
	{
		ifstream ifs("socks.conf");
		string rule;
		char mode = (cd == 0x01) ? 'c' : 'b';
		while (getline(ifs, rule))
		{
			if (rule[7] == mode)
			{
				string _ip = rule.substr(9, rule.size() - 1), str1, str2;
				if (!strcmp(_ip.c_str(), ip.c_str()))
				{
					ifs.close();
					return true;
				}
				stringstream ss1(_ip), ss2(ip);
				while (getline(ss1, str1, '.'))
				{
					getline(ss2, str2, '.');
					if (!strcmp(str1.c_str(), "*"))
					{
						ifs.close();
						return true;
					}
					if (strcmp(str1.c_str(), str2.c_str()))
						break;
				}
			}
		}
		ifs.close();
		return false;
	}

	void print_server_msg(string srcip, string srcport, string dstip, string dstport, string cmd, string reply)
	{
		cout << "<S_IP>: " << srcip << endl;
		cout << "<S_PORT>: " << srcport << endl;
		cout << "<D_IP>: " << dstip << endl;
		cout << "<D_PORT>: " << dstport << endl;
		cout << "<COMMAND>: " << cmd << endl;
		cout << "<Reply>: " << reply << endl;
		cout << "======================================" << endl;
	}
};

class SOCKS4Server
{
private:
	ip::tcp::acceptor _acceptor;
	ip::tcp::socket _socket;

public:
	SOCKS4Server(short port)
		: _acceptor(global_io_service, ip::tcp::endpoint(ip::tcp::v4(), port)),
		  _socket(global_io_service)
	{
		do_accept();
	}

private:
	void do_accept()
	{
		_acceptor.async_accept(_socket, [this](boost::system::error_code ec) {
			if (!ec)
			{
				global_io_service.notify_fork(io_service::fork_prepare);
				pid_t pid = fork();
				if (pid == 0)
				{
					global_io_service.notify_fork(io_service::fork_child);
					_acceptor.close();
					make_shared<SOCKS4Session>(move(_socket))->start();
				}
				else if (pid > 0)
				{
					global_io_service.notify_fork(io_service::fork_parent);
					_socket.close();
					do_accept();
				}
			}
		});
	}
};

int main(int argc, char *const argv[])
{
	if (argc != 2)
	{
		cerr << "Usage:" << argv[0] << " [port]" << endl;
		return 1;
	}

	try
	{
		unsigned short port = atoi(argv[1]);
		SOCKS4Server server(port);
		global_io_service.run();
	}
	catch (exception &e)
	{
		cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
