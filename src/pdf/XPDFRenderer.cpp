/*
 * Copyright (C) 2015-2018 Département de l'Instruction Publique (DIP-SEM)
 *
 * Copyright (C) 2013 Open Education Foundation
 *
 * Copyright (C) 2010-2013 Groupement d'Intérêt Public pour
 * l'Education Numérique en Afrique (GIP ENA)
 *
 * This file is part of OpenBoard.
 *
 * OpenBoard is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License,
 * with a specific linking exception for the OpenSSL project's
 * "OpenSSL" library (or with modified versions of it that use the
 * same license as the "OpenSSL" library).
 *
 * OpenBoard is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenBoard. If not, see <http://www.gnu.org/licenses/>.
 */




#include "XPDFRenderer.h"

#include <QtGui>

#include <frameworks/UBPlatformUtils.h>
#ifndef USE_XPDF
    #include <poppler/cpp/poppler-version.h>
#endif

#include "core/memcheck.h"

#ifdef XPDFRENDERER_CACHE_ZOOM_IMAGE
#ifdef XPDFRENDERER_CACHE_ZOOM_WITH_LOSS
double const XPDFRenderer::sRatioZoomRendering[] = { 3.0 };
#else //XPDFRENDERER_CACHE_ZOOM_WITH_LOSS
double const XPDFRenderer::sRatioZoomRendering[] = { 2.5, 5, 10.0 };
#endif //XPDFRENDERER_CACHE_ZOOM_WITH_LOSS
#endif //XPDFRENDERER_CACHE_ZOOM_IMAGE

QAtomicInt XPDFRenderer::sInstancesCount = 0;

XPDFRenderer::XPDFRenderer(const QString &filename, bool importingFile) :
#ifndef XPDFRENDERER_CACHE_ZOOM_IMAGE
    mSplash(nullptr),
#endif //XPDFRENDERER_CACHE_ZOOM_IMAGE
    mDocument(0)
{
#ifdef XPDFRENDERER_CACHE_ZOOM_IMAGE
    for (int i = 0; i < NbrZoomCache; i++)
    {
        m_cache.push_back(TypeCacheData(sRatioZoomRendering[i]));
    }
#endif //XPDFRENDERER_CACHE_ZOOM_IMAGE

    Q_UNUSED(importingFile);
    if (!globalParams)
    {
        // globalParams must be allocated once and never be deleted
        // note that this is *not* an instance variable of this XPDFRenderer class
#if POPPLER_VERSION_MAJOR > 0 || POPPLER_VERSION_MINOR >= 83
        globalParams = std::make_unique<GlobalParams>();
#else
        globalParams = new GlobalParams(0);
#endif
        globalParams->setupBaseFonts(QFile::encodeName(UBPlatformUtils::applicationResourcesDirectory() + "/" + "fonts").data());
    }
#ifdef USE_XPDF
    mDocument = new PDFDoc(new GString(filename.toLocal8Bit()), 0, 0, 0); // the filename GString is deleted on PDFDoc desctruction
#else
    mDocument = new PDFDoc(new GooString(filename.toLocal8Bit()), 0, 0, 0); // the filename GString is deleted on PDFDoc desctruction
#endif
    sInstancesCount.ref();
}

XPDFRenderer::~XPDFRenderer()
{
#ifdef XPDFRENDERER_CACHE_ZOOM_IMAGE
    for(int i = 0; i < m_cache.size(); i++)
    {
        TypeCacheData &cacheData = m_cache[i];
        if(cacheData.splash != nullptr){
            cacheData.cachedImage = QImage(); // The 'cachedImage' uses a buffer from 'splash'.
            delete cacheData.splash;
            cacheData.splash = nullptr;
        }
    }
#endif //XPDFRENDERER_CACHE_ZOOM_IMAGE

    if (mDocument)
    {
        delete mDocument;
        sInstancesCount.deref();
    }

    if (sInstancesCount.loadAcquire() == 0 && globalParams)
    {
#if POPPLER_VERSION_MAJOR > 0 || POPPLER_VERSION_MINOR >= 83
        globalParams.reset();
#else
        delete globalParams;
        globalParams = 0;
#endif
    }
}

bool XPDFRenderer::isValid() const
{
    if (mDocument)
    {
        return mDocument->isOk();
    }
    else
    {
        return false;
    }
}

int XPDFRenderer::pageCount() const
{
    if (isValid())
        return mDocument->getNumPages();
    else
        return 0;
}

QString XPDFRenderer::title() const
{
    if (isValid())
    {
#if POPPLER_VERSION_MAJOR > 0 || POPPLER_VERSION_MINOR >= 55
        Object pdfInfo = mDocument->getDocInfo();
#else
        Object pdfInfo;
        mDocument->getDocInfo(&pdfInfo);
#endif
        if (pdfInfo.isDict())
        {
            Dict *infoDict = pdfInfo.getDict();
#if POPPLER_VERSION_MAJOR > 0 || POPPLER_VERSION_MINOR >= 55
            Object title = infoDict->lookup((char*)"Title");
#else
            Object title;
            infoDict->lookup((char*)"Title", &title);
#endif
            if (title.isString())
            {
#if POPPLER_VERSION_MAJOR > 0 || POPPLER_VERSION_MINOR >= 72
                return QString(title.getString()->c_str());
#else
                return QString(title.getString()->getCString());
#endif
            }
        }
    }

    return QString();
}


QSizeF XPDFRenderer::pageSizeF(int pageNumber) const
{
    qreal cropWidth = 0;
    qreal cropHeight = 0;

    if (isValid())
    {
        int rotate = mDocument->getPageRotate(pageNumber);

        cropWidth = mDocument->getPageCropWidth(pageNumber) * this->dpiForRendering / 72.0;
        cropHeight = mDocument->getPageCropHeight(pageNumber) * this->dpiForRendering / 72.0;

        if (rotate == 90 || rotate == 270)
        {
            //switching width and height
            qreal tmpVar = cropWidth;
            cropWidth = cropHeight;
            cropHeight = tmpVar;
        }
    }
    return QSizeF(cropWidth, cropHeight);
}


int XPDFRenderer::pageRotation(int pageNumber) const
{
    if (mDocument)
        return  mDocument->getPageRotate(pageNumber);
    else
        return 0;
}

#ifndef XPDFRENDERER_CACHE_ZOOM_IMAGE
void XPDFRenderer::render(QPainter *p, int pageNumber, const QRectF &bounds)
{
    if (isValid())
    {
        qreal xscale = p->worldTransform().m11();
        qreal yscale = p->worldTransform().m22();

        QImage *pdfImage = createPDFImage(pageNumber, xscale, yscale, bounds);
        QTransform savedTransform = p->worldTransform();
        p->resetTransform();
        p->drawImage(QPointF(savedTransform.dx() + mSliceX, savedTransform.dy() + mSliceY), *pdfImage);
        p->setWorldTransform(savedTransform);
        delete pdfImage;
    }
}

QImage* XPDFRenderer::createPDFImage(int pageNumber, qreal xscale, qreal yscale, const QRectF &bounds)
{
    if (isValid())
    {
        SplashColor paperColor = {0xFF, 0xFF, 0xFF}; // white
        if(mSplash)
            delete mSplash;
        mSplash = new SplashOutputDev(splashModeRGB8, 1, false, paperColor);
#ifdef USE_XPDF
        mSplash->startDoc(mDocument->getXRef());
#else
        mSplash->startDoc(mDocument);
#endif
        int rotation = 0; // in degrees (get it from the worldTransform if we want to support rotation)
        bool useMediaBox = false;
        bool crop = true;
        bool printing = false;
        mSliceX = 0.;
        mSliceY = 0.;

        if (bounds.isNull())
        {
            mDocument->displayPage(mSplash, pageNumber, this->dpiForRendering * xscale, this->dpiForRendering *yscale,
                                   rotation, useMediaBox, crop, printing);
        }
        else
        {
            mSliceX = bounds.x() * xscale;
            mSliceY = bounds.y() * yscale;
            qreal sliceW = bounds.width() * xscale;
            qreal sliceH = bounds.height() * yscale;

            mDocument->displayPageSlice(mSplash, pageNumber, this->dpiForRendering * xscale, this->dpiForRendering * yscale,
                rotation, useMediaBox, crop, printing, mSliceX, mSliceY, sliceW, sliceH);
        }

        mpSplashBitmap = mSplash->getBitmap();
    }
    return new QImage(mpSplashBitmap->getDataPtr(), mpSplashBitmap->getWidth(), mpSplashBitmap->getHeight(), mpSplashBitmap->getWidth() * 3, QImage::Format_RGB888);
}
#else


void XPDFRenderer::render(QPainter *p, int pageNumber, const QRectF &bounds)
{
    Q_UNUSED(bounds);
    if (isValid())
    {
        qreal xscale = p->worldTransform().m11();
        qreal yscale = p->worldTransform().m22();
        Q_ASSERT(qFuzzyCompare(xscale, yscale)); // Zoom equal in all axes expected.
        Q_ASSERT(xscale > 0.0); // Potential Div0 later if this assert fail.

        int zoomIndex = 0;
        bool foundIndex = false;
        for (; zoomIndex < NbrZoomCache && !foundIndex;)
        {
            if (xscale <= m_cache[zoomIndex].ratio) {
                foundIndex = true;
            } else {
                zoomIndex++;
            }
        }

        if (!foundIndex) // Use the previous one.
            zoomIndex--;

        QImage const &pdfImage = createPDFImage(pageNumber, m_cache[zoomIndex]);
        QTransform savedTransform = p->worldTransform();

        double const ratioDifferenceBetweenWorldAndImage = 1.0/m_cache[zoomIndex].ratio;
        // The 'pdfImage' is rendered with a quality equal or superior. We adjust the 'transform' to zoom it
        // out the required ratio.
        QTransform newTransform = savedTransform.scale(ratioDifferenceBetweenWorldAndImage, ratioDifferenceBetweenWorldAndImage);
        p->setWorldTransform(newTransform);
        p->drawImage(QPointF( mSliceX,  mSliceY), pdfImage);

        p->setWorldTransform(savedTransform);
    }
}

QImage& XPDFRenderer::createPDFImage(int pageNumber, TypeCacheData &cacheData)
{
    if (isValid())
    {      
        SplashColor paperColor = {0xFF, 0xFF, 0xFF}; // white
        bool const requireUpdateImage = (pageNumber != cacheData.cachedPageNumber) || (cacheData.splash == nullptr);
        if (requireUpdateImage)
        {
            if(cacheData.splash != nullptr)
            {
                cacheData.cachedImage = QImage(); // The 'm_cachedImage' uses a buffer from 'mSplash'.
                delete cacheData.splash;
            }
            cacheData.splash = new SplashOutputDev(splashModeRGB8, 1, false, paperColor);
            cacheData.cachedPageNumber = pageNumber;

#ifdef USE_XPDF
            cacheData.splash->startDoc(mDocument->getXRef());
#else
            cacheData.splash->startDoc(mDocument);
#endif
            int rotation = 0; // in degrees (get it from the worldTransform if we want to support rotation)
            bool useMediaBox = false;
            bool crop = true;
            bool printing = false;
            mSliceX = 0.;
            mSliceY = 0.;

            mDocument->displayPage(cacheData.splash, pageNumber, this->dpiForRendering * cacheData.ratio, this->dpiForRendering * cacheData.ratio,
                                   rotation, useMediaBox, crop, printing);
            cacheData.splashBitmap = cacheData.splash->getBitmap();
        }

        // Note this uses the 'mSplash->getBitmap()->getDataPtr()' as data buffer.
        cacheData.cachedImage = QImage(cacheData.splashBitmap->getDataPtr(), cacheData.splashBitmap->getWidth(), cacheData.splashBitmap->getHeight(),
                               cacheData.splashBitmap->getWidth() * 3 /* bytesPerLine, 24 bits for RGB888, = 3 bytes */,
                               QImage::Format_RGB888);
    } else {
        cacheData.cachedImage = QImage();
    }

    return cacheData.cachedImage;
}
#endif
