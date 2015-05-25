#include "PDF.h"
#include "ByteArrayInputStream.h"
#include "DataInputStream.h"
#include "FilterFactory.h"
#include "Object.h"
#include "ObjReader.h"
#include "Xref.h"
#include <stdio.h>
#include <string.h>

PDF::PDF(InputStream *pSource)
{
	char str[40];
	int i, j, n, nOffset;
	bool bFound;
	Dictionary *pDict;
	Trailer *pTrailer;
	Object *pObj;

	m_pSource = new DataInputStream(pSource);
	*str = m_pSource->Read();
	m_pSource->ReadStr(str + 1, sizeof(str) - 1);
	if (strncmp(str, "%PDF-", 5) != 0)
	{
		m_pVersion[0] = '\0';
		delete m_pSource;
		m_pSource = NULL;
		return;
	}

	strcpy(m_pVersion, (const char *)str + 5);

	n = strlen("startxref");
	bFound = false;
	m_pSource->Seek(-(int)sizeof(str), SEEK_END);
	for (i = 30; i > 0; --i)
	{
		m_pSource->Read(str, sizeof(str));
		for (j = 0; j < sizeof(str) - n; ++j)
			if (strncmp(str + j, "startxref", n) == 0)
			{
				bFound = true;
				m_pSource->Seek(j - sizeof(str), SEEK_CUR);
				m_pSource->ReadStr(str, sizeof(str));  // startxref
				nOffset = m_pSource->ReadInt();  // offset of xref
				break;
			}
		if (bFound)
			break;
		m_pSource->Seek(n - sizeof(str) * 2, SEEK_CUR);
	}
	if (!bFound)  // startxref is not found
	{
		m_pVersion[0] = '\0';
		delete m_pSource;
		m_pSource = NULL;
		return;
	}

	m_pXref = new Xref();
	m_pReader = new ObjReader(m_pSource, m_pXref);
	m_pTrailer = NULL;
	do
	{
		pTrailer = new Trailer;
		m_pSource->Seek(nOffset, SEEK_SET);
		m_pSource->ReadStr(str, sizeof(str));
		if (strcmp(str, "xref") == 0)
		{
			m_pXref->Read(m_pSource);
			m_pSource->ReadStr(str, sizeof(str));  //trailer
			pTrailer->pStream = NULL;
			pTrailer->pDict = (Dictionary *)m_pReader->ReadObj();
		}
		else
		{
			m_pSource->ReadInt();
			m_pSource->ReadStr(str, sizeof(str));  //obj
			pTrailer->pStream = (Stream *)m_pReader->ReadObj();  //stream
			pTrailer->pDict = pTrailer->pStream->GetDictionary();
			pSource = CreateInputStream(pTrailer->pStream);
			m_pXref->Read(pSource);
			delete pSource;
		}

		pTrailer->pPrev = NULL;
		if (m_pTrailer == NULL)
			m_pTrailer = pTrailer;
		else
			m_pTail->pPrev = pTrailer;
		m_pTail = pTrailer;

		pObj = pTrailer->pDict->GetValue("Prev");
		if (pObj != NULL)
			nOffset = ((Numeric *)pObj)->GetValue();
	} while (pObj != NULL);

	m_nPageNum = 0;
	for (pTrailer = m_pTrailer; pTrailer != NULL; pTrailer = pTrailer->pPrev)
	{
		pObj = GetObject(pTrailer->pDict->GetValue("Root"));
		if (pObj != NULL)
		{
			m_pPages = (Dictionary *)GetObject(((Dictionary *)pObj)->GetValue("Pages"));
			m_nPageNum = ((Numeric *)GetObject(m_pPages->GetValue("Count")))->GetValue();
			break;
		}
	}
}

PDF::~PDF()
{
	if (m_pSource != NULL)
	{
		while (m_pTrailer != NULL)
		{
			m_pTail = m_pTrailer->pPrev;
			if (m_pTrailer->pStream != NULL)
				delete m_pTrailer->pStream;
			else
				delete m_pTrailer->pDict;
			delete m_pTrailer;
			m_pTrailer = m_pTail;
		}
		delete m_pReader;
		delete m_pXref;
		delete m_pSource;
	}
}

const char *PDF::GetVersion(void)
{
	return m_pVersion;
}

Xref *PDF::GetXref(void)
{
	return m_pXref;
}

Trailer *PDF::GetTrailer(void)
{
	return m_pTrailer;
}

Object *PDF::GetObject(int nNum)
{
	return m_pReader->ReadIndirectObj(nNum, 0);
}

Object *PDF::GetObject(Object *pObj)
{
	Object *pTarget;

	while (pObj != NULL && pObj->GetType() == Object::OBJ_REFERENCE)
	{
		pTarget = ((Reference *)pObj)->GetObject();
		if (pTarget == NULL)
		{
			pTarget = GetObject(((Reference *)pObj)->GetObjNum());
			((Reference *)pObj)->SetObject(pTarget);
		}
		pObj = pTarget;
	}
	return pObj;
}

InputStream *PDF::CreateInputStream(Stream *pStream)
{
	InputStream *pSource;
	Dictionary *pDict;
	Object *pFilter, *pParms;
	int i, n;

	pSource = new ByteArrayInputStream(pStream->GetValue(), pStream->GetSize());
	pDict = pStream->GetDictionary();
	pFilter = GetObject(pDict->GetValue("Filter"));
	if (pFilter != NULL)
	{
		pParms = GetObject(pDict->GetValue("DecodeParms"));
		if (pFilter->GetType() == Object::OBJ_NAME)
			pSource = FilterFactory::Create(((Name *)pFilter)->GetValue(), (Dictionary *)pParms, pSource);
		else if (pFilter->GetType() == Object::OBJ_ARRAY)
		{
			n = ((Array *)pFilter)->GetSize();
			for (i = 0; i < n; i++)
				pSource = FilterFactory::Create(((Name *)GetObject(((Array *)pFilter)->GetValue(i)))->GetValue(), pParms == NULL? NULL : (Dictionary *)((Array *)pParms)->GetValue(i), pSource);
		}
	}
	return pSource;
}

int PDF::GetPageNum(void)
{
	return m_nPageNum;
}

Dictionary *PDF::GetPage(int nIndex)
{
	return nIndex > 0? GetPage(m_pPages, nIndex) : NULL;
}

Dictionary *PDF::GetPage(Dictionary *pParent, int nIndex)
{
	Array *pKids;
	int i, nSize, nCount;
	Reference *pRef;
	Dictionary *pChild, *pRet;

	pKids = (Array *)pParent->GetValue("Kids");
	nSize = pKids->GetSize();
	for (i = 0; i < nSize; i++)
	{
		pRef = (Reference *)pKids->GetValue(i);
		pChild = (Dictionary *)GetObject(pRef->GetObjNum());
		if (strcmp(((Name *)pChild->GetValue("Type"))->GetValue(), "Pages") == 0)  //not leaf node
		{
			nCount = ((Numeric *)pChild->GetValue("Count"))->GetValue();
			if (nIndex <= nCount)
			{
				pRet = GetPage(pChild, nIndex);
				delete pChild;
				return pRet;
			}
			else
				nIndex -= nCount;
		}
		else if (--nIndex == 0)
			return pChild;
		delete pChild;
	}
	return NULL;
}
