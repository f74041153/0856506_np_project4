#include <array>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <fstream>

#define MAXHOST 5

using namespace std;
using namespace boost::asio;

struct RemoteServer
{
	string host;
	string port;
	string file;
	int id;
};

struct SServer
{
	string host;
	string port;
};

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
	SOCKS4Request(string dstip, string dstport, unsigned char cd) : _vn(0x04), _cd(cd), _null(0x00)
	{
		_dstport[0] = (stoi(dstport) >> 8) & 0xff;
		_dstport[1] = stoi(dstport) & 0xff;
		_dstip = ip::address_v4::from_string(dstip).to_bytes();
	}

	boost::array<mutable_buffer, 6> getbuffs()
	{

		_buffs = {
			buffer(&_vn, 1),
			buffer(&_cd, 1),
			buffer(_dstport),
			buffer(_dstip),
			buffer(&_null, 1)};
		return _buffs;
	}

	char getcd()
	{
		return _cd;
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
	SOCKS4Reply() {}

	boost::array<mutable_buffer, 4> getbuffs()
	{
		_buffs = {buffer(&_vn, 1),
				  buffer(&_cd, 1),
				  buffer(_dstport),
				  buffer(_dstip)};
		return _buffs;
	}

	char getcd()
	{
		return _cd;
	}
};

class ConsoleSession : public enable_shared_from_this<ConsoleSession>
{
private:
	enum
	{
		normal = 0,
		socks4 = 1,
		max_length = 4096
	};
	ip::tcp::resolver resolv{global_io_service};
	ip::tcp::socket tcp_socket{global_io_service};
	array<char, max_length> _data;
	struct SServer sserver;
	struct RemoteServer RS;
	ifstream testcase;
	int mode;
	SOCKS4Reply socks4_reply;

public:
	ConsoleSession(struct SServer sserv, struct RemoteServer rs) : sserver(sserv), RS(rs), testcase("test_case/" + rs.file) {}

	void start()
	{
		if (sserver_selected(sserver))
		{
			mode = socks4;
			ip::tcp::resolver::query q(sserver.host, sserver.port);
			do_resolve(q);
		}
		else
		{
			mode = normal;
			ip::tcp::resolver::query q(RS.host, RS.port);
			do_resolve(q);
		}
	}

private:
	void do_resolve(ip::tcp::resolver::query q)
	{
		auto self(shared_from_this());
		resolv.async_resolve(q, [this, self](boost::system::error_code ec, ip::tcp::resolver::iterator it) {
			if (!ec)
				do_connect(it);
		});
	}
	void do_connect(ip::tcp::resolver::iterator it)
	{
		auto self(shared_from_this());
		tcp_socket.async_connect(*it, [this, self](boost::system::error_code ec) {
			if (!ec)
			{
				if (mode == normal)
				{
					do_read();
				}
				else if (mode == socks4)
				{
					ip::tcp::resolver resolver(global_io_service);
					ip::tcp::resolver::query query(RS.host, RS.port);
					ip::tcp::endpoint endpoint = *resolver.resolve(query);
					string dstip = endpoint.address().to_string();
					string dstport = to_string(endpoint.port());
					do_socks4_request(dstip, dstport);
				}
			}
			else
			{
				tcp_socket.close();
			}
		});
	}

	void do_socks4_request(string dstip, string dstport)
	{
		SOCKS4Request socks4_req(dstip, dstport, 0x01);
		auto self(shared_from_this());
		tcp_socket.async_send(
			socks4_req.getbuffs(),
			[this, self](boost::system::error_code ec, size_t /* length */) {
				if (!ec)
					wait_socks4_reply();
			});
	}

	void wait_socks4_reply()
	{
		auto self(shared_from_this());
		tcp_socket.async_read_some(
			socks4_reply.getbuffs(),
			[this, self](boost::system::error_code ec, size_t length) {
				if (!ec)
				{
					char cd = socks4_reply.getcd();
					if (cd == 0x5a)
					{
						// request grant
						do_read();
					}
					else
					{
						tcp_socket.close();
					}
				}
			});
	}

	void do_read()
	{
		auto self(shared_from_this());
		tcp_socket.async_read_some(
			buffer(_data, max_length),
			[this, self](boost::system::error_code ec, size_t length) {
				if (!ec)
				{
					string shell2client(_data.data(), length);
					output_shell(RS.id, shell2client);
					if (percent_exist(shell2client))
					{
						string cmd;
						getline(testcase, cmd);
						cmd += "\n";
						output_command(RS.id, cmd);
						do_write(cmd);
					}
					else
					{
						do_read();
					}
				}
				else
				{
					testcase.close();
				}
			});
	}

	void do_write(string to_rs)
	{
		auto self(shared_from_this());
		tcp_socket.async_send(
			buffer(to_rs),
			[this, self](boost::system::error_code ec, size_t /* length */) {
				if (!ec)
					do_read();
			});
	}

	bool sserver_selected(struct SServer sserver)
	{
		if (sserver.host.size() && sserver.port.size())
			return true;
		return false;
	}

	bool percent_exist(string str)
	{
		for (unsigned int i = 0; i < str.size(); i++)
		{
			if (str[i] == '%')
				return true;
		}
		return false;
	}

	void html_escape(string &data)
	{
		string buffer;
		buffer.reserve(data.size());
		for (size_t pos = 0; pos != data.size(); ++pos)
		{
			switch (data[pos])
			{
			case '&':
				buffer.append("&amp;");
				break;
			case '\"':
				buffer.append("&quot;");
				break;
			case '\'':
				buffer.append("&apos;");
				break;
			case '<':
				buffer.append("&lt;");
				break;
			case '>':
				buffer.append("&gt;");
				break;
			case '\n':
				buffer.append("&NewLine;");
				break;
			case '\r':
				buffer.append("");
				break;
			default:
				buffer.append(&data[pos], 1);
				break;
			}
		}
		data.swap(buffer);
	}

	void output_shell(int rsid, string content)
	{
		// client <- console <- RS
		html_escape(content);
		string rsp = "<script>document.getElementById('s" + to_string(rsid) + "').innerHTML += \'" + content + "\';</script>";
		write(1, rsp.c_str(), rsp.size());
	}

	void output_command(int rsid, string content)
	{
		// client <- console -> RS
		html_escape(content);
		string rsp = "<script>document.getElementById('s" + to_string(rsid) + "').innerHTML += \'" + content + "\';</script>";
		write(1, rsp.c_str(), rsp.size());
	}
};

void set_remote_server(struct SServer &sserver, struct RemoteServer rs[])
{
	string q_str = getenv("QUERY_STRING");
	stringstream ss1(q_str);
	string str;
	for (int i = 0; i < MAXHOST; i++)
	{
		getline(ss1, str, '&');
		rs[i].host = str.substr(3, str.size() - 1);
		getline(ss1, str, '&');
		rs[i].port = str.substr(3, str.size() - 1);
		getline(ss1, str, '&');
		rs[i].file = str.substr(3, str.size() - 1);
		rs[i].id = i;
	}
	getline(ss1, str, '&');
	sserver.host = str.substr(3, str.size() - 1);
	getline(ss1, str, '&');
	sserver.port = str.substr(3, str.size() - 1);
}

bool rs_selected(struct RemoteServer rs)
{
	if (rs.host.size() && rs.port.size() && rs.file.size())
		return true;
	return false;
}

void first_page(struct RemoteServer rs[])
{
	string rsp = "Content-Type: text/html\n\n"
				 "<!DOCTYPE html>\n"
				 "<html lang=\"en\">\n"
				 "  <head>\n"
				 "    <meta charset=\"UTF-8\" />\n"
				 "    <title>NP Project 3 Console</title>\n"
				 "    <link\n"
				 "      rel=\"stylesheet\"\n"
				 "      href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\"\n"
				 "      integrity=\"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\"\n"
				 "      crossorigin=\"anonymous\"\n"
				 "    />\n"
				 "    <link\n"
				 "      href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\"\n"
				 "      rel=\"stylesheet\"\n"
				 "    />\n"
				 "    <link\n"
				 "      rel=\"icon\"\n"
				 "      type=\"image/png\"\n"
				 "      href=\"https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png\"\n"
				 "    />\n"
				 "    <style>\n"
				 "      * {\n"
				 "        font-family: 'Source Code Pro', monospace;\n"
				 "        font-size: 1rem !important;\n"
				 "      }\n"
				 "      body {\n"
				 "        background-color: #212529;\n"
				 "      }\n"
				 "      pre {\n"
				 "        color: #cccccc;\n"
				 "      }\n"
				 "      b {\n"
				 "        color: #ffffff;\n"
				 "      }\n"
				 "    </style>\n"
				 "  </head>\n"
				 "  <body>\n"
				 "    <table class=\"table table-dark table-bordered\">\n"
				 "      <thead>\n"
				 "        <tr>\n";

	for (int i = 0; i < MAXHOST; i++)
	{
		if (rs_selected(rs[i]))
		{
			rsp += "          <th scope=\"col\">" + rs[i].host + ":" + rs[i].port + "</th>\n";
		}
	}

	rsp += "        </tr>\n"
		   "      </thead>\n"
		   "      <tbody>\n"
		   "        <tr>\n";

	for (int i = 0; i < MAXHOST; i++)
	{
		if (rs_selected(rs[i]))
		{
			rsp += "          <td><pre id=\"s" + to_string(i) + "\" class=\"mb-0\"></pre></td>\n";
		}
	}

	rsp += "        </tr>\n"
		   "      </tbody>\n"
		   "    </table>\n"
		   "  </body>\n"
		   "</html>";

	write(1, rsp.c_str(), rsp.size());
}

int main(int argc, char *const argv[])
{

	try
	{
		struct RemoteServer RS[MAXHOST];
		struct SServer sserver;
		set_remote_server(sserver, RS);
		first_page(RS);
		for (int i = 0; i < MAXHOST; i++)
		{
			if (rs_selected(RS[i]))
			{
				make_shared<ConsoleSession>(sserver, RS[i])->start();
			}
		}
		global_io_service.run();
	}
	catch (exception &e)
	{
		cerr << "Exception: " << e.what() << "\n";
	}
	return 0;
}
