/*
  The contents of this file are dedicated by all of its authors, including

    Michael S. Gashler,
    Eric Moyer,
    anonymous contributors,

  to the public domain (http://creativecommons.org/publicdomain/zero/1.0/).

  Note that some moral obligations still exist in the absence of legal ones.
  For example, it would still be dishonest to deliberately misrepresent the
  origin of a work. Although we impose no legal requirements to obtain a
  license, it is beseeming for those who build on the works of others to
  give back useful improvements, or find a way to pay it forward. If
  you would like to cite us, a published paper about Waffles can be found
  at http://jmlr.org/papers/volume12/gashler11a/gashler11a.pdf. If you find
  our code to be useful, the Waffles team would love to hear how you use it.
*/

#include "GTokenizer.h"
#include "GError.h"
#include "GHolders.h"
#ifndef MIN_PREDICT
#include "GFile.h"
#include "GString.h"
#endif // MIN_PREDICT
#include "GBitTable.h"
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <errno.h>
#ifdef WINDOWS
	#include <io.h>
#else
	#include <unistd.h>
#endif

using std::string;
using std::map;
using std::string;

namespace GClasses {


GCharSet::GCharSet(const char* szChars)
	: m_bt(256)
{
	char c = '\0';
	while(*szChars != '\0')
	{
		if(*szChars == '-')
		{
			if(c == '\0')
				m_bt.set((unsigned char)*szChars);
			else
			{
				char d = szChars[1];
				if(d <= c)
					throw Ex("invalid character range");
				for(c++; c <= d && c != 0; c++)
					m_bt.set((unsigned char)c);
				szChars++;
			}
		}
		else
			m_bt.set((unsigned char)*szChars);
		c = *szChars;
		szChars++;
	}
}

bool GCharSet::find(char c)
{
	return m_bt.bit((unsigned char)c);
}


GTokenizer::GTokenizer(const char* szFilename)
{
	std::ifstream* pStream = new std::ifstream();
	m_pStream = pStream;
	pStream->exceptions(std::ios::badbit);
	try
	{
		pStream->open(szFilename, std::ios::binary);
	}
	catch(const std::exception&)
	{
		throw Ex("Error while trying to open the file, ", szFilename, ". ", strerror(errno));
	}
	m_pBufStart = new char[256];
	m_pBufPos = m_pBufStart;
	m_pBufEnd = m_pBufStart + 256;
	m_lineCol = 0;
	m_line = 1;
}

GTokenizer::GTokenizer(const char* pFile, size_t len)
{
	if(len > 0)
	{
		m_pStream = new std::istringstream(string(pFile, len));
	}
	else
	{
		m_pStream = new std::istringstream(pFile);
	}
	m_pBufStart = new char[256];
	m_pBufPos = m_pBufStart;
	m_pBufEnd = m_pBufStart + 256;
	m_lineCol = 0;
	m_line = 1;
}

GTokenizer::~GTokenizer()
{
	delete[] m_pBufStart;
	delete(m_pStream);
}

void GTokenizer::growBuf()
{
	size_t len = m_pBufEnd - m_pBufStart;
	char* pNewBuf = new char[len * 2];
	m_pBufEnd = pNewBuf + (len * 2);
	memcpy(pNewBuf, m_pBufStart, len);
	m_pBufPos = pNewBuf + len;
	delete[] m_pBufStart;
	m_pBufStart = pNewBuf;
}

char GTokenizer::get()
{
	char c = m_pStream->get();
	if(c == '\n')
	{
		m_line++;
		m_lineCol = 0;
	} else {
		m_lineCol++;
	}
	return c;
}

void GTokenizer::bufferChar(char c)
{
	if(m_pBufPos == m_pBufEnd)
		growBuf();
	*m_pBufPos = c;
	m_pBufPos++;
}

char* GTokenizer::nullTerminate()
{
	if(m_pBufPos == m_pBufEnd)
		growBuf();
	*m_pBufPos = '\0';
	return m_pBufStart;
}

char* GTokenizer::appendToToken(const char* string)
{
	while(*string != '\0')
	{
		bufferChar(*string);
		string++;
	}
	return nullTerminate();
}

char* GTokenizer::nextUntil(GCharSet& delimeters, size_t minLen)
{
	m_pBufPos = m_pBufStart;
	while(has_more())
	{
		char c = m_pStream->peek();
		if(delimeters.find(c))
			break;
		c = get();
		bufferChar(c);
	}
	if((size_t)(m_pBufPos - m_pBufStart) < minLen)
		throw Ex("On line ", to_str(m_line), ", col ", to_str(col()), ", expected a token of at least size ", to_str(minLen), ", but got only ", to_str(m_pBufPos - m_pBufStart));
	return nullTerminate();
}

char* GTokenizer::nextUntilNotEscaped(char escapeChar, GCharSet& delimeters)
{
	m_pBufPos = m_pBufStart;
	char cCur = '\0';
	while(has_more())
	{
		char c = m_pStream->peek();
		if(delimeters.find(c) && cCur != escapeChar)
			break;
		c = get();
		bufferChar(c);
		cCur = c;
	}
	return nullTerminate();
}

char* GTokenizer::nextWhile(GCharSet& set, size_t minLen)
{
	m_pBufPos = m_pBufStart;
	while(has_more())
	{
		char c = m_pStream->peek();
		if(!set.find(c))
			break;
		c = get();
		bufferChar(c);
	}
	if((size_t)(m_pBufPos - m_pBufStart) < minLen)
		throw Ex("Unexpected token on line ", to_str(m_line), ", col ", to_str(col()));
	return nullTerminate();
}

void GTokenizer::skip(GCharSet& delimeters)
{
	while(has_more())
	{
		char c = m_pStream->peek();
		if(!delimeters.find(c))
			break;
		c = get();
	}
}

void GTokenizer::skipTo(GCharSet& delimeters)
{
	while(has_more())
	{
		char c = m_pStream->peek();
		if(delimeters.find(c))
			break;
		c = get();
	}
}

char* GTokenizer::nextArg(GCharSet& delimiters, char escapeChar)
{
	m_pBufPos = m_pBufStart;
	char c = m_pStream->peek();
	if(c == '"')
	{
		bufferChar('"');
		advance(1);
		GCharSet cs("\"\n");
		while(has_more())
		{
			char c = m_pStream->peek();
			if(cs.find(c))
				break;
			c = get();
			bufferChar(c);
		}
		if(peek() != '"')
			throw Ex("Expected matching double-quotes on line ",
								 to_str(m_line), ", col ", to_str(col()));
		bufferChar('"');
		advance(1);
		while(!delimiters.find(m_pStream->peek()))
			advance(1);
		return nullTerminate();
	}
	else if(c == '\'')
	{
		bufferChar('\'');
		advance(1);
		GCharSet cs("'\n");
		while(has_more())
		{
			char c = m_pStream->peek();
			if(cs.find(c))
				break;
			c = get();
			bufferChar(c);
		}
		if(peek() != '\'')
			throw Ex("Expected a matching single-quote on line ", to_str(m_line),
								 ", col ", to_str(col()));
		bufferChar('\'');
		advance(1);
		while(!delimiters.find(m_pStream->peek()))
			advance(1);
		return nullTerminate();
	}

	bool inEscapeMode = false;
	while(has_more())
	{
		char c = m_pStream->peek();
		if(inEscapeMode)
		{
			if(c == '\n')
			{
				throw Ex("Error: '", to_str(escapeChar), "' character used as "
									 "last character on a line to attempt to extend string over "
									 "two lines on line" , to_str(m_line), ", col ",
									 to_str(col()) );
			}
			c = get();
			bufferChar(c);
			inEscapeMode = false;
		}
		else
		{
			if(c == '\n' || delimiters.find(c)){ break; }
			c = get();
			if(c == escapeChar)	{	inEscapeMode = true;	}
			else { bufferChar(c);	}
		}
	}

	return nullTerminate();
}

void GTokenizer::advance(size_t n)
{
	while(n > 0 && has_more())
	{
		get();
		n--;
	}
}

char GTokenizer::peek()
{
	if(has_more())
		return m_pStream->peek();
	else
		return '\0';
}

size_t GTokenizer::line()
{
	return m_line;
}

bool GTokenizer::has_more()
{
	return (m_pStream->peek() != EOF);
}

void GTokenizer::expect(const char* szString)
{
	while(*szString != '\0' && has_more())
	{
		char c = get();
		if(c != *szString)
			throw Ex("Expected \"", szString, "\" on line ", to_str(m_line), ", col ", to_str(col()));
		szString++;
	}
	if(*szString != '\0')
		throw Ex("Expected \", szString, \". Reached end-of-file instead.");
}

size_t GTokenizer::tokenLength()
{
	return m_pBufPos - m_pBufStart;
}

char* GTokenizer::trim(GCharSet& set)
{
	char* pStart = m_pBufStart;
	while(pStart < m_pBufPos && set.find(*pStart))
		pStart++;
	for(char* pEnd = m_pBufPos - 1; pEnd >= pStart && set.find(*pEnd); pEnd--)
		*pEnd = '\0';
	return pStart;
}

size_t GTokenizer::col()
{
	return m_lineCol;
}

} // namespace GClasses
