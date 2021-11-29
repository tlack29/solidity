/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
#include <solc/LSPTCPTransport.h>

#include <libsolutil/JSON.h>
#include <fmt/format.h>

#include <optional>
#include <string>
#include <iostream>

namespace solidity::lsp
{

using std::nullopt;
using std::optional;
using std::string_view;

using namespace std::string_literals;

LSPTCPTransport::LSPTCPTransport(unsigned short _port, std::string const& _address):
	m_io_service(),
	m_endpoint(boost::asio::ip::make_address(_address), _port),
	m_acceptor(m_io_service),
	m_stream(),
	m_jsonTransport()
{
	m_acceptor.open(m_endpoint.protocol());
	m_acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
	m_acceptor.bind(m_endpoint);
	m_acceptor.listen();
}

bool LSPTCPTransport::closed() const noexcept
{
	return !m_acceptor.is_open();
}

optional<Json::Value> LSPTCPTransport::receive()
{
	auto const clientClosed = [&]() { return !m_stream || !m_stream.value().good() || m_stream.value().eof(); };

	if (clientClosed())
	{
		m_stream.emplace(m_acceptor.accept());
		if (clientClosed())
			return nullopt;

		m_jsonTransport.emplace(m_stream.value(), m_stream.value());
	}

	if (auto value = m_jsonTransport.value().receive(); value.has_value())
		return value;

	if (clientClosed())
	{
		m_jsonTransport.reset();
		m_stream.reset();
	}
	return nullopt;
}

void LSPTCPTransport::notify(std::string _method, Json::Value _params)
{
	if (m_jsonTransport.has_value())
		m_jsonTransport.value().notify(move(_method), _params);
}

void LSPTCPTransport::reply(MessageID _id, Json::Value _result)
{
	if (m_jsonTransport.has_value())
		m_jsonTransport.value().reply(_id, _result);
}

void LSPTCPTransport::error(MessageID _id, ErrorCode _code, std::string _message)
{
	if (m_jsonTransport.has_value())
		m_jsonTransport.value().error(_id, _code, move(_message));
}

}
