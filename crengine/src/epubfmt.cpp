#include "../include/epubfmt.h"


bool DetectEpubFormat( LVStreamRef stream )
{





    LVContainerRef m_arc = LVOpenArchieve( stream );
    if ( m_arc.isNull() )
        return false; // not a ZIP archive

    // read "mimetype" file contents from root of archive
    lString16 mimeType;
    {
        LVStreamRef mtStream = m_arc->OpenStream(L"mimetype", LVOM_READ );
        if ( !mtStream.isNull() ) {
            int size = mtStream->GetSize();
            if ( size>4 && size<100 ) {
                LVArray<char> buf( size+1, '\0' );
                if ( mtStream->Read( buf.get(), size, NULL )==LVERR_OK ) {
                    for ( int i=0; i<size; i++ )
                        if ( buf[i]<32 || ((unsigned char)buf[i])>127 )
                            buf[i] = 0;
                    buf[size] = 0;
                    if ( buf[0] )
                        mimeType = Utf8ToUnicode( lString8( buf.get() ) );
                }
            }
        }
    }

    if ( mimeType != L"application/epub+zip" )
        return false;
    return true;
}

bool ImportEpubDocument( LVStreamRef stream, ldomDocument * m_doc, LVDocViewCallback * progressCallback )
{
    LVContainerRef m_arc = LVOpenArchieve( stream );
    if ( m_arc.isNull() )
        return false; // not a ZIP archive

    // check root media type
    lString16 rootfilePath;
    lString16 rootfileMediaType;
    // read container.xml
    {
        LVStreamRef container_stream = m_arc->OpenStream(L"META-INF/container.xml", LVOM_READ);
        if ( !container_stream.isNull() ) {
            ldomDocument * doc = LVParseXMLStream( container_stream );
            if ( doc ) {
                ldomNode * rootfile = doc->nodeFromXPath( lString16(L"container/rootfiles/rootfile") );
                if ( rootfile && rootfile->isElement() ) {
                    rootfilePath = rootfile->getAttributeValue(L"full-path");
                    rootfileMediaType = rootfile->getAttributeValue(L"media-type");
                }
                delete doc;
            }
        }
    }

    if ( rootfilePath.empty() || rootfileMediaType!=L"application/oebps-package+xml" )
        return false;


    // read content.opf
    EpubItems epubItems;
    //EpubItem * epubToc = NULL; //TODO
    LVArray<EpubItem*> spineItems;
    lString16 codeBase;
    lString16 css;

    //
    {
        int lastSlash = -1;
        for ( int i=0; i<(int)rootfilePath.length(); i++ )
            if ( rootfilePath[i]=='/' )
                lastSlash = i;
        if ( lastSlash>0 )
            codeBase = lString16( rootfilePath.c_str(), lastSlash + 1);
    }

    LVStreamRef content_stream = m_arc->OpenStream(rootfilePath.c_str(), LVOM_READ);
    if ( content_stream.isNull() )
        return false;

    // reading content stream
    {
        ldomDocument * doc = LVParseXMLStream( content_stream );
        if ( !doc )
            return false;

        CRPropRef m_doc_props = doc->getProps();
        m_doc_props->setString(DOC_PROP_TITLE, doc->textFromXPath( lString16(L"package/metadata/title") ));
        m_doc_props->setString(DOC_PROP_AUTHORS, doc->textFromXPath( lString16(L"package/metadata/creator") ));

        // items
        for ( int i=1; i<50000; i++ ) {
            ldomNode * item = doc->nodeFromXPath( lString16(L"package/manifest/item[") + lString16::itoa(i) + L"]" );
            if ( !item )
                break;
            lString16 href = item->getAttributeValue(L"href");
            lString16 mediaType = item->getAttributeValue(L"media-type");
            lString16 id = item->getAttributeValue(L"id");
            if ( !href.empty() && !id.empty() ) {
                EpubItem * epubItem = new EpubItem;
                epubItem->href = href;
                epubItem->id = id;
                epubItem->mediaType = mediaType;
                epubItems.add( epubItem );
            }
            if ( mediaType==L"text/css" ) {
                lString16 name = codeBase + href;
                LVStreamRef cssStream = m_arc->OpenStream(name.c_str(), LVOM_READ);
                if ( !cssStream.isNull() ) {
                    css << L"\n";
                    css << LVReadTextFile( cssStream );
                }
            }
        }

        // spine == itemrefs
        if ( epubItems.length()>0 ) {
            ldomNode * spine = doc->nodeFromXPath( lString16(L"package/spine") );
            if ( spine ) {


                //EpubItem * epubToc = epubItems.findById( spine->getAttributeValue(L"toc") ); //TODO
                for ( int i=1; i<50000; i++ ) {
                    ldomNode * item = doc->nodeFromXPath( lString16(L"package/spine/itemref[") + lString16::itoa(i) + L"]" );
                    if ( !item )
                        break;
                    EpubItem * epubItem = epubItems.findById( item->getAttributeValue(L"idref") );
                    if ( epubItem ) {
                        // TODO: add to document
                        spineItems.add( epubItem );
                    }
                }
            }
        }
        delete doc;
    }

    if ( spineItems.length()==0 )
        return false;

    lUInt32 saveFlags = m_doc ? m_doc->getDocFlags() : DOC_FLAG_DEFAULTS;
    m_doc->setDocFlags( saveFlags );
    m_doc->setContainer( m_arc );

    ldomDocumentWriter writer(m_doc);
#if 0
    m_doc->setNodeTypes( fb2_elem_table );
    m_doc->setAttributeTypes( fb2_attr_table );
    m_doc->setNameSpaceTypes( fb2_ns_table );
#endif
    m_doc->setCodeBase( codeBase );

    ldomDocumentFragmentWriter appender(&writer, lString16(L"body"));
    writer.OnStart(NULL);
    writer.OnTagOpenNoAttr(L"", L"body");
    int fragmentCount = 0;
    for ( int i=0; i<spineItems.length(); i++ ) {
        if ( spineItems[i]->mediaType==L"application/xhtml+xml" ) {
            lString16 name = codeBase + spineItems[i]->href;
            {
                CRLog::debug("Checking fragment: %s", LCSTR(name));
                LVStreamRef stream = m_arc->OpenStream(name.c_str(), LVOM_READ);
                if ( !stream.isNull() ) {
                    LVXMLParser parser(stream, &appender);
                    if ( parser.CheckFormat() && parser.Parse() ) {
                        // valid
                        fragmentCount++;
                    } else {
                        CRLog::error("Document type is not XML/XHTML for fragment %s", LCSTR(name));
                    }
                }
            }
        }
    }
    writer.OnTagClose(L"", L"body");
    writer.OnStop();
    CRLog::debug("EPUB: %d documents merged", fragmentCount);

    if ( fragmentCount==0 )
        return false;

    // set stylesheet
    //m_doc->getStyleSheet()->clear();
    m_doc->setStyleSheet( NULL, true );
    //m_doc->getStyleSheet()->parse(m_stylesheet.c_str());
    if ( !css.empty() && m_doc->getDocFlag(DOC_FLAG_ENABLE_INTERNAL_STYLES) ) {

        m_doc->setStyleSheet( "p.p { text-align: justify }\n"
            "svg { text-align: center }\n"
            "i { display: inline; font-style: italic }\n"
            "b { display: inline; font-weight: bold }\n"
            "abbr { display: inline }\n"
            "acronym { display: inline }\n"
            "address { display: inline }\n"
            "p.title-p { hyphenate: none }\n"
//abbr, acronym, address, blockquote, br, cite, code, dfn, div, em, h1, h2, h3, h4, h5, h6, kbd, p, pre, q, samp, span, strong, var
        , false);
        m_doc->setStyleSheet( UnicodeToUtf8(css).c_str(), false );
        //m_doc->getStyleSheet()->parse(UnicodeToUtf8(css).c_str());
    } else {
        //m_doc->getStyleSheet()->parse(m_stylesheet.c_str());
        //m_doc->setStyleSheet( m_stylesheet.c_str(), false );
    }
#if 0
    LVStreamRef out = LVOpenFileStream( L"c:\\doc.xml" , LVOM_WRITE );
    if ( !out.isNull() )
        m_doc->saveToStream( out, "utf-8" );
#endif

    // DONE!
    if ( progressCallback ) {
        progressCallback->OnLoadFileEnd( );
        m_doc->compact();
        m_doc->dumpStatistics();
    }
    return true;

}
