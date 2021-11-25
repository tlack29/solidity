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
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTUtils.h>
#include <libsolidity/ast/ASTVisitor.h>
#include <libsolidity/interface/ReadFile.h>
#include <libsolidity/interface/StandardCompiler.h>
#include <libsolidity/lsp/LanguageServer.h>

#include <liblangutil/SourceReferenceExtractor.h>
#include <liblangutil/CharStream.h>

#include <libsolutil/Visitor.h>
#include <libsolutil/JSON.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <fmt/format.h>

#include <ostream>
#include <string>

using namespace std;
using namespace std::placeholders;

using namespace solidity::lsp;
using namespace solidity::langutil;
using namespace solidity::frontend;

namespace
{

void log(string const& _message)
{
#if 0
	static ofstream logFile("/tmp/solc.lsp.log", std::ios::app);
	logFile << _message << endl;
#else
	(void)_message;
#endif
}

Json::Value toJson(LineColumn _pos)
{
	Json::Value json = Json::objectValue;
	json["line"] = max(_pos.line, 0);
	json["character"] = max(_pos.column, 0);

	return json;
}

Json::Value toJsonRange(LineColumn const& _start, LineColumn const& _end)
{
	Json::Value json;
	json["start"] = toJson(_start);
	json["end"] = toJson(_end);
	return json;
}

constexpr int toDiagnosticSeverity(Error::Type _errorType)
{
	// 1=Error, 2=Warning, 3=Info, 4=Hint
	switch (Error::errorSeverity(_errorType))
	{
	case Error::Severity::Error: return 1;
	case Error::Severity::Warning: return 2;
	case Error::Severity::Info: return 3;
	}
	return 1;
}

}

LanguageServer::LanguageServer(Transport& _transport):
	m_client{_transport},
	m_handlers{
		{"$/cancelRequest", [](auto, auto) {/*nothing for now as we are synchronous */} },
		{"cancelRequest", [](auto, auto) {/*nothing for now as we are synchronous */} },
		{"exit", [this](auto, auto) { m_exitRequested = true; }},
		{"initialize", bind(&LanguageServer::handleInitialize, this, _1, _2)},
		{"initialized", [](auto, auto) {} },
		{"shutdown", [this](auto, auto) { m_shutdownRequested = true; }},
		{"textDocument/didChange", bind(&LanguageServer::handleTextDocumentDidChange, this, _1, _2)},
		{"textDocument/didClose", [](auto, auto) {/*nothing for now*/}},
		{"textDocument/didOpen", bind(&LanguageServer::handleTextDocumentDidOpen, this, _1, _2)},
		{"workspace/didChangeConfiguration", bind(&LanguageServer::handleWorkspaceDidChangeConfiguration, this, _1, _2)},
	},
	m_fileReader{"/"},
	m_compilerStack{bind(&FileReader::readFile, ref(m_fileReader), _1, _2)}

{
}

DocumentPosition LanguageServer::extractDocumentPosition(Json::Value const& _json) const
{
	DocumentPosition dpos{};

	dpos.path = _json["textDocument"]["uri"].asString();
	dpos.position.line = _json["position"]["line"].asInt();
	dpos.position.column = _json["position"]["character"].asInt();

	return dpos;
}

Json::Value LanguageServer::toRange(SourceLocation const& _location) const
{
	if (_location.start < 0 || _location.end < 0)
		return toJsonRange({}, {});

	solAssert(_location.sourceName, "");
	CharStream const& stream = m_compilerStack.charStream(*_location.sourceName);
	LineColumn start = stream.translatePositionToLineColumn(_location.start);
	LineColumn end = stream.translatePositionToLineColumn(_location.end);
	return toJsonRange(start, end);
}

Json::Value LanguageServer::toJson(SourceLocation const& _location) const
{
	solAssert(_location.sourceName);
	Json::Value item = Json::objectValue;
	item["uri"] = sourceUnitNameToClientPath(*_location.sourceName);
	item["range"] = toRange(_location);
	return item;
}

string LanguageServer::clientPathToSourceUnitName(string const& _path) const
{
	// TOOD test with with base path set.
	string path = _path;
	if (path.find("file://") == 0)
		path = path.substr(7);
	// TODO this still uses the CWD in some circumstances.
	return m_fileReader.cliPathToSourceUnitName(path);
}

string LanguageServer::sourceUnitNameToClientPath(string const& _sourceUnitName) const
{
	// TOOD test with with base path set.
	return "file://" + (m_fileReader.basePath() / _sourceUnitName).generic_string();
}

bool LanguageServer::clientPathSourceKnown(string const& _path) const
{
	return m_fileReader.sourceCodes().count(clientPathToSourceUnitName(_path));
}

void LanguageServer::changeConfiguration(Json::Value const& _settings)
{
	m_settingsObject = _settings;
}

bool LanguageServer::compile(string const& _path)
{
	// TODO: optimize! do not recompile if nothing has changed (file(s) not flagged dirty).

	if (!clientPathSourceKnown(_path))
		return false;

	m_compilerStack.reset(false);
	m_compilerStack.setSources(m_fileReader.sourceCodes());
	m_compilerStack.compile(CompilerStack::State::AnalysisPerformed);

	return true;
}

void LanguageServer::compileSourceAndReport(string const& _path)
{
	compile(_path);

	map<string, Json::Value> diagnosticsBySourceUnit;
	for (string const& sourceUnitName: m_fileReader.sourceCodes() | ranges::views::keys)
		diagnosticsBySourceUnit[sourceUnitName] = Json::arrayValue;

	for (shared_ptr<Error const> const& error: m_compilerStack.errors())
	{
		SourceLocation const* location = error->sourceLocation();
		if (!location || !location->sourceName)
			// LSP only has diagnostics applied to individual files.
			continue;

		Json::Value jsonDiag;
		jsonDiag["source"] = "solc";
		jsonDiag["severity"] = toDiagnosticSeverity(error->type());
		jsonDiag["code"] = Json::UInt64{error->errorId().error};
		string message = error->typeName() + ":";
		if (string const* comment = error->comment())
			message += " " + *comment;
		jsonDiag["message"] = move(message);
		jsonDiag["range"] = toRange(*location);

		if (auto const* secondary = error->secondarySourceLocation())
			for (auto&& [secondaryMessage, secondaryLocation]: secondary->infos)
			{
				Json::Value jsonRelated;
				jsonRelated["message"] = secondaryMessage;
				jsonRelated["location"] = toJson(secondaryLocation);
				jsonDiag["relatedInformation"].append(jsonRelated);
			}

		diagnosticsBySourceUnit[*location->sourceName].append(jsonDiag);
	}

	for (string const& sourceUnitName: m_fileReader.sourceCodes() | ranges::views::keys)
	{
		Json::Value params;
		params["uri"] = sourceUnitNameToClientPath(sourceUnitName);
		params["diagnostics"] = move(diagnosticsBySourceUnit.at(sourceUnitName));
		m_client.notify("textDocument/publishDiagnostics", move(params));
	}
}

bool LanguageServer::run()
{
	while (!m_exitRequested && !m_client.closed())
	{
		optional<Json::Value> const jsonMessage = m_client.receive();
		if (!jsonMessage)
			continue;

		try
		{
			string const methodName = (*jsonMessage)["method"].asString();
			MessageID const id = (*jsonMessage)["id"];

			if (auto handler = valueOrDefault(m_handlers, methodName))
				handler(id, (*jsonMessage)["params"]);
			else
				m_client.error(id, ErrorCode::MethodNotFound, "Unknown method " + methodName);
		}
		catch (exception const& e)
		{
			log("Unhandled exception caught when handling message. "s + e.what());
		}
	}
	return m_shutdownRequested;
}

void LanguageServer::handleInitialize(MessageID _id, Json::Value const& _args)
{
	// The default of FileReader is to use `.`, but the path from where the LSP was started
	// should not matter.
	string rootPath("/");
	if (Json::Value uri = _args["rootUri"])
		rootPath = uri.asString();
	else if (Json::Value rootPath = _args["rootPath"])
		rootPath = rootPath.asString();

	//log("root path: " + rootPath);
	// TODO the lsp client probably uses file://... for the root path
	m_fileReader.setBasePath(boost::filesystem::path(rootPath));
	if (_args["initializationOptions"].isObject())
		changeConfiguration(_args["initializationOptions"]);

	Json::Value replyArgs;
	replyArgs["serverInfo"]["name"] = "solc";
	replyArgs["serverInfo"]["version"] = string(VersionNumber);
	replyArgs["capabilities"]["textDocumentSync"]["openClose"] = true;
	replyArgs["capabilities"]["textDocumentSync"]["change"] = 2; // 0=none, 1=full, 2=incremental

	m_client.reply(_id, replyArgs);
}

void LanguageServer::handleWorkspaceDidChangeConfiguration(MessageID, Json::Value const& _args)
{
	if (_args["settings"].isObject())
		changeConfiguration(_args["settings"]);
}

void LanguageServer::handleTextDocumentDidOpen(MessageID /*_id*/, Json::Value const& _args)
{
	if (!_args["textDocument"])
		return;

	auto const text = _args["textDocument"]["text"].asString();
	auto uri = _args["textDocument"]["uri"].asString();
	m_fileReader.setSourceDirectly(clientPathToSourceUnitName(uri), text);
	compileSourceAndReport(uri);
}

void LanguageServer::handleTextDocumentDidChange(MessageID /*_id*/, Json::Value const& _args)
{
	auto const uri = _args["textDocument"]["uri"].asString();
	auto const contentChanges = _args["contentChanges"];

	for (Json::Value jsonContentChange: contentChanges)
	{
		// TODO all the errors here have to be reported to the client.
		if (!jsonContentChange.isObject()) // Protocol error, will only happen on broken clients, so silently ignore it.
			continue;

		if (!clientPathSourceKnown(uri))
			// should be an error as well
			continue;

		string text = jsonContentChange["text"].asString();
		if (!jsonContentChange["range"].isObject()) // full content update
		{
			m_fileReader.setSourceDirectly(clientPathToSourceUnitName(uri), move(text));
			continue;
		}

		Json::Value const jsonRange = jsonContentChange["range"];
		// TODO could use a general helper to read line/characer json objects into int pairs or whateveer
		int const startLine = jsonRange["start"]["line"].asInt();
		int const startColumn = jsonRange["start"]["character"].asInt();
		int const endLine = jsonRange["end"]["line"].asInt();
		int const endColumn = jsonRange["end"]["character"].asInt();

		string buffer = m_fileReader.sourceCodes().at(clientPathToSourceUnitName(uri));
		optional<int> const startOpt = CharStream::translateLineColumnToPosition(buffer, startLine, startColumn);
		optional<int> const endOpt = CharStream::translateLineColumnToPosition(buffer, endLine, endColumn);
		if (!startOpt || !endOpt)
			continue;

		size_t const start = static_cast<size_t>(startOpt.value());
		size_t const count = static_cast<size_t>(endOpt.value()) - start; // TODO: maybe off-by-1 bug? +1 missing?
		buffer.replace(start, count, move(text));
		m_fileReader.setSourceDirectly(clientPathToSourceUnitName(uri), move(buffer));
	}

	if (!contentChanges.empty())
		compileSourceAndReport(uri);
}
