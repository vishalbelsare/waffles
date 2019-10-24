/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or pay it forward in their own field. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/


#include <GClasses/GHolders.h>
#include <GClasses/GHtml.h>
#include <GClasses/GText.h>
#include <GClasses/GTime.h>
#include <GClasses/GFile.h>
#include <vector>
#include <sstream>
#include "forum.h"
#include "server.h"

using std::ostream;
using std::string;
using std::vector;
using std::cout;

void Forum::format_comment_recursive(GDomNode* pEntry, std::ostream& os, std::string& id, bool allow_direct_reply, size_t depth)
{
	// Extract relevant data
	const char* szUsername = pEntry->getString("user");
	const char* szDate = pEntry->getString("date");
	const char* szComment = pEntry->getString("comment");
	GDomNode* pReplies = pEntry->getIfExists("replies");

	// Add the comment enclosed in a "bubble" div
	os << "<div class=\"bubble\"><table cellpadding=10px><tr>\n";
	os << "<td valign=top align=right>";
	os << szUsername << "<br>";
	os << szDate;
	os << "</td><td valign=top>";
	os << szComment;
	os << "<br><a href=\"#javascript:void(0)\" onclick=\"tog_viz('" << id << "')\">reply</a>";
	os << "</td></tr>\n";
	os << "</table></div><br>\n";

	if(depth > 0)
	{
		// Recursively add replies
		if(pReplies)
		{
			os << "<div class=\"indent\">";
			for(size_t i = 0; i < pReplies->size(); i++)
			{
				string child_id = id;
				child_id += "_";
				child_id += to_str(i);
				bool child_allow_direct_replies = true;
				if(i == pReplies->size() - 1)
					child_allow_direct_replies = false;
				format_comment_recursive(pReplies->get(i), os, child_allow_direct_replies ? child_id : id, child_allow_direct_replies, depth - 1);
			}
			os << "</div>\n";
		}

		if(allow_direct_reply)
		{
			// Add a hidden div with a reply field and post button
			os << "<div class=\"hidden indent\" id=\"" << id << "\"><textarea id=\"" << id << "t\" rows=\"2\" cols=\"50\"></textarea><br>";
			os << "<button type=\"button\" onclick=\"post_comment('" << id << "t')\">Post</button><br><br></div>\n";
		}
	}
}


void Forum::ajaxGetForumHtml(Server* pServer, GDynamicPageSession* pSession, const GDomNode* pIn, GDom& doc, GDomNode* pOut)
{
	// Request the whole file
	GJsonAsADatabase& jaad = pServer->jaad();
	const GDomNode* pResponse = jaad.apply(pIn->getString("file"), "", &doc);
	std::ostringstream os;
	if(pResponse)
	{
		// Convert to HTML
		if(pResponse->type() == GDomNode::type_list)
		{
			// Convert hierarchical list of comments into HTML
			os << "<br><br><h2>Visitor Comments:</h2>\n";
			for(size_t i = 0; i < pResponse->size(); i++)
			{
				GDomNode* pEntry = pResponse->get(i);
				string id = "r";
				id += to_str(i);
				format_comment_recursive(pEntry, os, id, true, 12);
			}
			os << "<textarea id=\"rt\" rows=\"2\" cols=\"50\"></textarea><br>\n";
			os << "<input type=\"button\" onclick=\"post_comment('rt');\" value=\"Post\">\n";
		}
		else
		{
			// Convert error message into HTML
			os << "<br><font color=\"red\">[Comments currently unavailable because: ";
			os << pResponse->getString("jaad_error");
			os << "]</font><br>\n";
		}
	}
	else
	{
		os << "<br><br><h2>Visitor Comments:</h2>\n";
		os << "[No comments yet.]<br>\n";
		os << "<textarea id=\"rt\" rows=\"2\" cols=\"50\"></textarea><br>\n";
		os << "<input type=\"button\" onclick=\"post_comment('rt');\" value=\"Post\">\n";
	}
	pOut->add(&doc, "html", os.str().c_str());
}

string HTML_scrub_string(const char* szString)
{
	std::ostringstream stream;
	while(*szString != '\0')
	{
		if(*szString == '&')
			stream << "&amp;";
		else if(*szString == '<')
			stream << "&lt;";
		else if(*szString == '>')
			stream << "&gt;";
		else
			stream << *szString;
		++szString;
	}
	return stream.str();
}

string JSON_encode_string(const char* szString)
{
	std::ostringstream stream;
	stream << '"';
	while(*szString != '\0')
	{
		if(*szString < ' ')
		{
			switch(*szString)
			{
				case '\b': stream << "\\b"; break;
				case '\f': stream << "\\f"; break;
				case '\n': stream << "\\n"; break;
				case '\r': stream << "\\r"; break;
				case '\t': stream << "\\t"; break;
				default:
					stream << (*szString);
			}
		}
		else if(*szString == '\\')
			stream << "\\\\";
		else if(*szString == '"')
			stream << "\\\"";
		else
			stream << (*szString);
		++szString;
	}
	stream << '"';
	return stream.str();
}

size_t portions(const char* szString, double* whitespace, double* letters, double* caps)
{
	size_t _letters = 0;
	size_t _caps = 0;
	size_t _chars = 0;
	size_t _space = 0;
	while(*szString != '\0')
	{
		if(*szString >= 'a' && *szString <= 'z')
			++_letters;
		else if(*szString >= 'A' && *szString <= 'Z')
		{
			++_letters;
			++_caps;
		}
		else if(*szString <= ' ')
			++_space;
		++_chars;
		++szString;
	}
	*whitespace = (double)_space / _chars;
	*letters = (double)_letters / _chars;
	*caps = (double)_caps / _letters;
	return _chars;
}

void Forum::ajaxAddComment(Server* pServer, GDynamicPageSession* pSession, const GDomNode* pIn, GDom& doc, GDomNode* pOut)
{
	// Get the data
	Account* pAccount = getAccount(pSession);
	if(!pAccount)
		throw Ex("You must be logged in to comment.");
	const char* szUsername = pAccount->username();
	const char* szFilename = pIn->getString("file");
	const char* szId = pIn->getString("id");
	const char* szIpAddress = pSession->connection()->getIPAddress();
	const char* szComment = pIn->getString("comment");

	// Evaluate the comment
	if(strstr(szComment, "://"))
		throw Ex("Comment rejected. Hyperlinks are not allowed.");
	if(strstr(szComment, "href="))
		throw Ex("Comment rejected. Hyperlinks are not allowed.");
	double _ws, _letters, _caps;
	size_t len = portions(szComment, &_ws, &_letters, &_caps);
	if(len > 3 && _ws > 0.5)
		throw Ex("Comment rejected. Too much whitespace.");
	if(len > 25 && _ws < 0.02)
		throw Ex("Comment rejected. Use more spaces.");
	if(_letters < 0.65)
		throw Ex("Comment rejected. Comments should be mostly words, not symbols");
	if(_caps > 0.2)
		throw Ex("Comment rejected. Using all-caps is not friendly.");

	// Parse the ID (to determine where to insert the comment)
	if(*szId != 'r')
		throw Ex("Invalid ID");
	if(*szId == '_')
		throw Ex("Invalid ID");
	szId++;
	std::ostringstream cmd;
	size_t depth = 0;
	while(true)
	{
		if(*szId == 't')
			break;
		else if(*szId == '_')
		{
			++szId;
			if(*szId == '_' || *szId == 't')
				throw Ex("Invalid ID");
		}
		else if(*szId >= '0' && *szId <= '9')
		{
			if(++depth > 20)
				throw Ex("Invalid ID");
			cmd << '[';
			while(*szId >= '0' && *szId <= '9')
			{
				cmd << *szId;
				++szId;
			}
			cmd << "].replies";
		}
		else
			throw Ex("Invalid ID");
	}

	// Construct the JAAD command
	string sDate;
	GTime::appendTimeStampValue(&sDate, "-", " ", ":", true);
	string encodedIP = JSON_encode_string(szIpAddress);
	string encodedUser = JSON_encode_string(szUsername);
	string encodedDate = JSON_encode_string(sDate.c_str());
	string encodedComment = JSON_encode_string(HTML_scrub_string(szComment).c_str());
	cmd << " += {\"ip\":" << encodedIP;
	cmd << ",\"user\":" << encodedUser;
	cmd << ",\"date\":" << encodedDate;
	cmd << ",\"comment\":" << encodedComment;
	cmd << "}";

	// Send the request
	GJsonAsADatabase& jaad = pServer->jaad();
	const GDomNode* pResponse = jaad.apply(szFilename, cmd.str().c_str(), &doc);
	pOut->add(&doc, "response", pResponse);

	// Log this comment
	std::ostringstream cmd2;
	cmd2 << "+={\"ip\":" << encodedIP;
	cmd2 << ",\"user\":" << encodedUser;
	cmd2 << ",\"date\":" << encodedDate;
	cmd2 << ",\"file\":" << JSON_encode_string(szFilename);
	cmd2 << ",\"comment\":" << encodedComment;
	cmd2 << "}";
	jaad.apply("comments.json", cmd2.str().c_str(), &doc);
}

void Forum::pageFeed(Server* pServer, GDynamicPageSession* pSession, ostream& response)
{
	// Check access privileges
	Account* pAccount = getAccount(pSession);
	if(!pAccount->isAdmin())
	{
		response << "Sorry, you must be an admin to access this page.";
		return;
	}

	// Load the log file
	string filename = pServer->m_basePath;
	filename += "comments_log.json";
	GDom dom;
	dom.loadJson(filename.c_str());
	GDomNode* pNode = dom.root();

	// Generate a page
	response << "<h2>Recent comments</h2>\n";
	response << "<table><tr><td>Ban user</td><td>Date</td><td>Username</td><td>IP</td><td>Comment</td></tr>\n";
	for(size_t i = 0; i < pNode->size(); i++)
	{
		GDomNode* pComment = pNode->get(i);
		response << "<tr><td><input type=\"checkbox\"></td>";
		response << "<td>" << pComment->getString("date") << "</td>";
		response << "<td>" << pComment->getString("user") << "</td>";
		response << "<td>" << pComment->getString("ip") << "</td>";
		response << "<td>" << pComment->getString("comment") << "</td>";
		response << "\n";
	}
	response << "</table>\n";
}

void Forum::pageForumWrapper(Server* pServer, GDynamicPageSession* pSession, ostream& response)
{
	// Parse the url
	string s = pSession->url();
	if(s.length() < 3 || s.substr(0, 3).compare("/c/") != 0)
		throw Ex("Unexpected url");
	s = s.substr(3);
	if(s[s.length() - 1] == '/')
		s += "index.html";
	PathData pd;
	GFile::parsePath(s.c_str(), &pd);
	if(pd.extStart == pd.len)
	{
		s += "/index.html";
		GFile::parsePath(s.c_str(), &pd);
	}

	// If it's not an HTML file, just send the file
	if(s.substr(pd.extStart).compare(".html") != 0)
	{
		pSession->connection()->sendFileSafe(pServer->m_basePath.c_str(), s.c_str(), response);
		return;
	}

	// Parse the HTML
	string fullPath = pServer->m_basePath;
	fullPath += s;
	GHtmlDoc doc(fullPath.c_str());
	GHtmlElement* pElHtml = doc.document()->childTag("html");
	if(!pElHtml)
		return throw Ex("Expected an html tag");
	GHtmlElement* pElHead = pElHtml->childTag("head");
	if(!pElHead)
		pElHead = new GHtmlElement(pElHtml, "head", 0);
	GHtmlElement* pElStyle = pElHead->childTag("style");
	if(!pElStyle)
		pElStyle = new GHtmlElement(pElHead, "style");
	GHtmlElement* pElBody = pElHtml->childTag("body");
	if(!pElBody)
		return throw Ex("Expected a body tag");

	// Inject the comments stuff
	string& sStyle = pServer->cache("chat_style.css");
	GHtmlElement* pAddedStyle = new GHtmlElement(pElStyle, sStyle.c_str());
	pAddedStyle->text = true;
	string sScript = "\nlet comments_file = \"";
	sScript += s.substr(0, pd.extStart);
	sScript += "_comments.json\";\n";
	sScript += pServer->cache("chat_script.js");
	GHtmlElement* pAddedScript = new GHtmlElement(pElBody, "script", 0);
	pAddedScript->addAttr("type", "\"text/javascript\"");
	GHtmlElement* pAddedScriptContent = new GHtmlElement(pAddedScript, sScript.c_str());
	pAddedScriptContent->text = true;
	GHtmlElement* pAddedComments = new GHtmlElement(pElBody, "div");
	pAddedComments->addAttr("id", "\"comments\"");

	doc.document()->write(response);
}
