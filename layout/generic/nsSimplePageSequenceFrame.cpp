/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSimplePageSequenceFrame.h"

#include "nsCOMPtr.h"
#include "nsPresContext.h"
#include "gfxContext.h"
#include "nsRenderingContext.h"
#include "nsGkAtoms.h"
#include "nsIPresShell.h"
#include "nsIPrintSettings.h"
#include "nsPageFrame.h"
#include "nsSubDocumentFrame.h"
#include "nsRegion.h"
#include "nsCSSFrameConstructor.h"
#include "nsContentUtils.h"
#include "nsDisplayList.h"
#include "nsHTMLCanvasFrame.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsIDateTimeFormat.h"
#include "nsServiceManagerUtils.h"
#include <algorithm>

// DateTime Includes
#include "nsDateTimeFormatCID.h"

#define OFFSET_NOT_SET -1

// Print Options
#include "nsIPrintOptions.h"

using namespace mozilla;
using namespace mozilla::dom;

static const char sPrintOptionsContractID[] = "@mozilla.org/gfx/printsettings-service;1";

//

#include "prlog.h"
#ifdef PR_LOGGING 
PRLogModuleInfo *
GetLayoutPrintingLog()
{
  static PRLogModuleInfo *sLog;
  if (!sLog)
    sLog = PR_NewLogModule("printing-layout");
  return sLog;
}
#define PR_PL(_p1)  PR_LOG(GetLayoutPrintingLog(), PR_LOG_DEBUG, _p1)
#else
#define PR_PL(_p1)
#endif

nsSimplePageSequenceFrame*
NS_NewSimplePageSequenceFrame(nsIPresShell* aPresShell, nsStyleContext* aContext)
{
  return new (aPresShell) nsSimplePageSequenceFrame(aContext);
}

NS_IMPL_FRAMEARENA_HELPERS(nsSimplePageSequenceFrame)

nsSimplePageSequenceFrame::nsSimplePageSequenceFrame(nsStyleContext* aContext) :
  nsContainerFrame(aContext),
  mTotalPages(-1),
  mSelectionHeight(-1),
  mYSelOffset(0),
  mCalledBeginPage(false),
  mCurrentCanvasListSetup(false)
{
  nscoord halfInch = PresContext()->CSSTwipsToAppUnits(NS_INCHES_TO_TWIPS(0.5));
  mMargin.SizeTo(halfInch, halfInch, halfInch, halfInch);

  // XXX Unsafe to assume successful allocation
  mPageData = new nsSharedPageData();
  mPageData->mHeadFootFont =
    *PresContext()->GetDefaultFont(kGenericFont_serif,
                                   aContext->StyleFont()->mLanguage);
  mPageData->mHeadFootFont.size = nsPresContext::CSSPointsToAppUnits(10);

  nsresult rv;
  mPageData->mPrintOptions = do_GetService(sPrintOptionsContractID, &rv);

  // Doing this here so we only have to go get these formats once
  SetPageNumberFormat("pagenumber",  "%1$d", true);
  SetPageNumberFormat("pageofpages", "%1$d of %2$d", false);
}

nsSimplePageSequenceFrame::~nsSimplePageSequenceFrame()
{
  delete mPageData;
  ResetPrintCanvasList();
}

NS_QUERYFRAME_HEAD(nsSimplePageSequenceFrame)
  NS_QUERYFRAME_ENTRY(nsIPageSequenceFrame)
NS_QUERYFRAME_TAIL_INHERITING(nsContainerFrame)

//----------------------------------------------------------------------

void
nsSimplePageSequenceFrame::SetDesiredSize(nsHTMLReflowMetrics& aDesiredSize,
                                          const nsHTMLReflowState& aReflowState,
                                          nscoord aWidth,
                                          nscoord aHeight)
{
    // Aim to fill the whole size of the document, not only so we
    // can act as a background in print preview but also handle overflow
    // in child page frames correctly.
    // Use availableWidth so we don't cause a needless horizontal scrollbar.
    aDesiredSize.Width() = std::max(aReflowState.AvailableWidth(),
                                nscoord(aWidth * PresContext()->GetPrintPreviewScale()));
    aDesiredSize.Height() = std::max(aReflowState.ComputedHeight(),
                                 nscoord(aHeight * PresContext()->GetPrintPreviewScale()));
}

void
nsSimplePageSequenceFrame::Reflow(nsPresContext*          aPresContext,
                                  nsHTMLReflowMetrics&     aDesiredSize,
                                  const nsHTMLReflowState& aReflowState,
                                  nsReflowStatus&          aStatus)
{
  NS_PRECONDITION(aPresContext->IsRootPaginatedDocument(),
                  "A Page Sequence is only for real pages");
  DO_GLOBAL_REFLOW_COUNT("nsSimplePageSequenceFrame");
  DISPLAY_REFLOW(aPresContext, this, aReflowState, aDesiredSize, aStatus);
  NS_FRAME_TRACE_REFLOW_IN("nsSimplePageSequenceFrame::Reflow");

  aStatus = NS_FRAME_COMPLETE;  // we're always complete

  // Don't do incremental reflow until we've taught tables how to do
  // it right in paginated mode.
  if (!(GetStateBits() & NS_FRAME_FIRST_REFLOW)) {
    // Return our desired size
    SetDesiredSize(aDesiredSize, aReflowState, mSize.width, mSize.height);
    aDesiredSize.SetOverflowAreasToDesiredBounds();
    FinishAndStoreOverflow(&aDesiredSize);
    return;
  }

  // See if we can get a Print Settings from the Context
  if (!mPageData->mPrintSettings &&
      aPresContext->Medium() == nsGkAtoms::print) {
      mPageData->mPrintSettings = aPresContext->GetPrintSettings();
  }

  // now get out margins & edges
  if (mPageData->mPrintSettings) {
    nsIntMargin unwriteableTwips;
    mPageData->mPrintSettings->GetUnwriteableMarginInTwips(unwriteableTwips);
    NS_ASSERTION(unwriteableTwips.left  >= 0 && unwriteableTwips.top >= 0 &&
                 unwriteableTwips.right >= 0 && unwriteableTwips.bottom >= 0,
                 "Unwriteable twips should be non-negative");

    nsIntMargin marginTwips;
    mPageData->mPrintSettings->GetMarginInTwips(marginTwips);
    mMargin = aPresContext->CSSTwipsToAppUnits(marginTwips + unwriteableTwips);

    int16_t printType;
    mPageData->mPrintSettings->GetPrintRange(&printType);
    mPrintRangeType = printType;

    nsIntMargin edgeTwips;
    mPageData->mPrintSettings->GetEdgeInTwips(edgeTwips);

    // sanity check the values. three inches are sometimes needed
    int32_t inchInTwips = NS_INCHES_TO_INT_TWIPS(3.0);
    edgeTwips.top    = clamped(edgeTwips.top,    0, inchInTwips);
    edgeTwips.bottom = clamped(edgeTwips.bottom, 0, inchInTwips);
    edgeTwips.left   = clamped(edgeTwips.left,   0, inchInTwips);
    edgeTwips.right  = clamped(edgeTwips.right,  0, inchInTwips);

    mPageData->mEdgePaperMargin =
      aPresContext->CSSTwipsToAppUnits(edgeTwips + unwriteableTwips);
  }

  // *** Special Override ***
  // If this is a sub-sdoc (meaning it doesn't take the whole page)
  // and if this Document is in the upper left hand corner
  // we need to suppress the top margin or it will reflow too small

  nsSize pageSize = aPresContext->GetPageSize();

  mPageData->mReflowSize = pageSize;
  // If we're printing a selection, we need to reflow with
  // unconstrained height, to make sure we'll get to the selection
  // even if it's beyond the first page of content.
  if (nsIPrintSettings::kRangeSelection == mPrintRangeType) {
    mPageData->mReflowSize.height = NS_UNCONSTRAINEDSIZE;
  }
  mPageData->mReflowMargin = mMargin;

  // We use the CSS "margin" property on the -moz-page pseudoelement
  // to determine the space between each page in print preview.
  // Keep a running y-offset for each page.
  nscoord y = 0;
  nscoord maxXMost = 0;

  // Tile the pages vertically
  nsHTMLReflowMetrics kidSize(aReflowState);
  for (nsIFrame* kidFrame = mFrames.FirstChild(); nullptr != kidFrame; ) {
    // Set the shared data into the page frame before reflow
    nsPageFrame * pf = static_cast<nsPageFrame*>(kidFrame);
    pf->SetSharedPageData(mPageData);

    // Reflow the page
    nsHTMLReflowState kidReflowState(aPresContext, aReflowState, kidFrame,
                                     LogicalSize(kidFrame->GetWritingMode(),
                                                 pageSize));
    nsReflowStatus  status;

    kidReflowState.SetComputedWidth(kidReflowState.AvailableWidth());
    //kidReflowState.SetComputedHeight(kidReflowState.AvailableHeight());
    PR_PL(("AV W: %d   H: %d\n", kidReflowState.AvailableWidth(), kidReflowState.AvailableHeight()));

    nsMargin pageCSSMargin = kidReflowState.ComputedPhysicalMargin();
    y += pageCSSMargin.top;
    const nscoord x = pageCSSMargin.left;

    // Place and size the page. If the page is narrower than our
    // max width then center it horizontally
    ReflowChild(kidFrame, aPresContext, kidSize, kidReflowState, x, y, 0, status);

    FinishReflowChild(kidFrame, aPresContext, kidSize, nullptr, x, y, 0);
    y += kidSize.Height();
    y += pageCSSMargin.bottom;

    maxXMost = std::max(maxXMost, x + kidSize.Width() + pageCSSMargin.right);

    // Is the page complete?
    nsIFrame* kidNextInFlow = kidFrame->GetNextInFlow();

    if (NS_FRAME_IS_FULLY_COMPLETE(status)) {
      NS_ASSERTION(!kidNextInFlow, "bad child flow list");
    } else if (!kidNextInFlow) {
      // The page isn't complete and it doesn't have a next-in-flow, so
      // create a continuing page.
      nsIFrame* continuingPage = aPresContext->PresShell()->FrameConstructor()->
        CreateContinuingFrame(aPresContext, kidFrame, this);

      // Add it to our child list
      mFrames.InsertFrame(nullptr, kidFrame, continuingPage);
    }

    // Get the next page
    kidFrame = kidFrame->GetNextSibling();
  }

  // Get Total Page Count
  nsIFrame* page;
  int32_t pageTot = 0;
  for (page = mFrames.FirstChild(); page; page = page->GetNextSibling()) {
    pageTot++;
  }

  // Set Page Number Info
  int32_t pageNum = 1;
  for (page = mFrames.FirstChild(); page; page = page->GetNextSibling()) {
    nsPageFrame * pf = static_cast<nsPageFrame*>(page);
    if (pf != nullptr) {
      pf->SetPageNumInfo(pageNum, pageTot);
    }
    pageNum++;
  }

  // Create current Date/Time String
  if (!mDateFormatter) {
    mDateFormatter = do_CreateInstance(NS_DATETIMEFORMAT_CONTRACTID);
  }
  if (!mDateFormatter) {
    return;
  }
  nsAutoString formattedDateString;
  time_t ltime;
  time( &ltime );
  if (NS_SUCCEEDED(mDateFormatter->FormatTime(nullptr /* nsILocale* locale */,
                                              kDateFormatShort,
                                              kTimeFormatNoSeconds,
                                              ltime,
                                              formattedDateString))) {
    SetDateTimeStr(formattedDateString);
  }

  // Return our desired size
  // Adjust the reflow size by PrintPreviewScale so the scrollbars end up the
  // correct size
  SetDesiredSize(aDesiredSize, aReflowState, maxXMost, y);

  aDesiredSize.SetOverflowAreasToDesiredBounds();
  FinishAndStoreOverflow(&aDesiredSize);

  // cache the size so we can set the desired size 
  // for the other reflows that happen
  mSize.width  = maxXMost;
  mSize.height = y;

  NS_FRAME_TRACE_REFLOW_OUT("nsSimplePageSequeceFrame::Reflow", aStatus);
  NS_FRAME_SET_TRUNCATION(aStatus, aReflowState, aDesiredSize);
}

//----------------------------------------------------------------------

#ifdef DEBUG_FRAME_DUMP
nsresult
nsSimplePageSequenceFrame::GetFrameName(nsAString& aResult) const
{
  return MakeFrameName(NS_LITERAL_STRING("SimplePageSequence"), aResult);
}
#endif

//====================================================================
//== Asynch Printing
//====================================================================
NS_IMETHODIMP
nsSimplePageSequenceFrame::GetCurrentPageNum(int32_t* aPageNum)
{
  NS_ENSURE_ARG_POINTER(aPageNum);

  *aPageNum = mPageNum;
  return NS_OK;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::GetNumPages(int32_t* aNumPages)
{
  NS_ENSURE_ARG_POINTER(aNumPages);

  *aNumPages = mTotalPages;
  return NS_OK;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::IsDoingPrintRange(bool* aDoing)
{
  NS_ENSURE_ARG_POINTER(aDoing);

  *aDoing = mDoingPageRange;
  return NS_OK;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::GetPrintRange(int32_t* aFromPage, int32_t* aToPage)
{
  NS_ENSURE_ARG_POINTER(aFromPage);
  NS_ENSURE_ARG_POINTER(aToPage);

  *aFromPage = mFromPageNum;
  *aToPage   = mToPageNum;
  return NS_OK;
}

// Helper Function
void 
nsSimplePageSequenceFrame::SetPageNumberFormat(const char* aPropName, const char* aDefPropVal, bool aPageNumOnly)
{
  // Doing this here so we only have to go get these formats once
  nsXPIDLString pageNumberFormat;
  // Now go get the Localized Page Formating String
  nsresult rv =
    nsContentUtils::GetLocalizedString(nsContentUtils::ePRINTING_PROPERTIES,
                                       aPropName, pageNumberFormat);
  if (NS_FAILED(rv)) { // back stop formatting
    pageNumberFormat.AssignASCII(aDefPropVal);
  }

  SetPageNumberFormat(pageNumberFormat, aPageNumOnly);
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::StartPrint(nsPresContext*    aPresContext,
                                      nsIPrintSettings* aPrintSettings,
                                      const nsAString&  aDocTitle,
                                      const nsAString&  aDocURL)
{
  NS_ENSURE_ARG_POINTER(aPresContext);
  NS_ENSURE_ARG_POINTER(aPrintSettings);

  if (!mPageData->mPrintSettings) {
    mPageData->mPrintSettings = aPrintSettings;
  }

  if (!aDocTitle.IsEmpty()) {
    mPageData->mDocTitle = aDocTitle;
  }
  if (!aDocURL.IsEmpty()) {
    mPageData->mDocURL = aDocURL;
  }

  aPrintSettings->GetStartPageRange(&mFromPageNum);
  aPrintSettings->GetEndPageRange(&mToPageNum);
  aPrintSettings->GetPageRanges(mPageRanges);

  mDoingPageRange = nsIPrintSettings::kRangeSpecifiedPageRange == mPrintRangeType ||
                    nsIPrintSettings::kRangeSelection == mPrintRangeType;

  // If printing a range of pages make sure at least the starting page
  // number is valid
  int32_t totalPages = mFrames.GetLength();

  if (mDoingPageRange) {
    if (mFromPageNum > totalPages) {
      return NS_ERROR_INVALID_ARG;
    }
  }

  // Begin printing of the document
  nsresult rv = NS_OK;

  // Determine if we are rendering only the selection
  aPresContext->SetIsRenderingOnlySelection(nsIPrintSettings::kRangeSelection == mPrintRangeType);


  if (mDoingPageRange) {
    // XXX because of the hack for making the selection all print on one page
    // we must make sure that the page is sized correctly before printing.
    nscoord height = aPresContext->GetPageSize().height;

    int32_t pageNum = 1;
    nscoord y = 0;//mMargin.top;

    for (nsIFrame* page = mFrames.FirstChild(); page;
         page = page->GetNextSibling()) {
      if (pageNum >= mFromPageNum && pageNum <= mToPageNum) {
        nsRect rect = page->GetRect();
        rect.y = y;
        rect.height = height;
        page->SetRect(rect);
        y += rect.height + mMargin.top + mMargin.bottom;
      }
      pageNum++;
    }

    // adjust total number of pages
    if (nsIPrintSettings::kRangeSelection != mPrintRangeType) {
      totalPages = pageNum - 1;
    }
  }

  mPageNum = 1;

  if (mTotalPages == -1) {
    mTotalPages = totalPages;
  }

  return rv;
}

void
GetPrintCanvasElementsInFrame(nsIFrame* aFrame, nsTArray<nsRefPtr<HTMLCanvasElement> >* aArr)
{
  if (!aFrame) {
    return;
  }
  for (nsIFrame::ChildListIterator childLists(aFrame);
    !childLists.IsDone(); childLists.Next()) {

    nsFrameList children = childLists.CurrentList();
    for (nsFrameList::Enumerator e(children); !e.AtEnd(); e.Next()) {
      nsIFrame* child = e.get();

      // Check if child is a nsHTMLCanvasFrame.
      nsHTMLCanvasFrame* canvasFrame = do_QueryFrame(child);

      // If there is a canvasFrame, try to get actual canvas element.
      if (canvasFrame) {
        HTMLCanvasElement* canvas =
          HTMLCanvasElement::FromContentOrNull(canvasFrame->GetContent());
        if (canvas && canvas->GetMozPrintCallback()) {
          aArr->AppendElement(canvas);
          continue;
        }
      }

      if (!child->GetFirstPrincipalChild()) {
        nsSubDocumentFrame* subdocumentFrame = do_QueryFrame(child);
        if (subdocumentFrame) {
          // Descend into the subdocument
          nsIFrame* root = subdocumentFrame->GetSubdocumentRootFrame();
          child = root;
        }
      }
      // The current child is not a nsHTMLCanvasFrame OR it is but there is
      // no HTMLCanvasElement on it. Check if children of `child` might
      // contain a HTMLCanvasElement.
      GetPrintCanvasElementsInFrame(child, aArr);
    }
  }
}

void
nsSimplePageSequenceFrame::DetermineWhetherToPrintPage()
{
  // See whether we should print this page
  mPrintThisPage = true;
  bool printEvenPages, printOddPages;
  mPageData->mPrintSettings->GetPrintOptions(nsIPrintSettings::kPrintEvenPages, &printEvenPages);
  mPageData->mPrintSettings->GetPrintOptions(nsIPrintSettings::kPrintOddPages, &printOddPages);

  // If printing a range of pages check whether the page number is in the
  // range of pages to print
  if (mDoingPageRange) {
    if (mPageNum < mFromPageNum) {
      mPrintThisPage = false;
    } else if (mPageNum > mToPageNum) {
      mPageNum++;
      mPrintThisPage = false;
      return;
    } else {
      int32_t length = mPageRanges.Length();
    
      // Page ranges are pairs (start, end)
      if (length && (length % 2 == 0)) {
        mPrintThisPage = false;
      
        int32_t i;
        for (i = 0; i < length; i += 2) {          
          if (mPageRanges[i] <= mPageNum && mPageNum <= mPageRanges[i+1]) {
            mPrintThisPage = true;
            break;
          }
        }
      }
    }
  }

  // Check for printing of odd and even pages
  if (mPageNum & 0x1) {
    if (!printOddPages) {
      mPrintThisPage = false;  // don't print odd numbered page
    }
  } else {
    if (!printEvenPages) {
      mPrintThisPage = false;  // don't print even numbered page
    }
  }
  
  if (nsIPrintSettings::kRangeSelection == mPrintRangeType) {
    mPrintThisPage = true;
  }
}

nsIFrame*
nsSimplePageSequenceFrame::GetCurrentPageFrame()
{
  int32_t i = 1;
  for (nsFrameList::Enumerator childFrames(mFrames); !childFrames.AtEnd();
       childFrames.Next()) {
    if (i == mPageNum) {
      return childFrames.get();
    }
    ++i;
  }
  return nullptr;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::PrePrintNextPage(nsITimerCallback* aCallback, bool* aDone)
{
  nsIFrame* currentPage = GetCurrentPageFrame();
  if (!currentPage) {
    *aDone = true;
    return NS_ERROR_FAILURE;
  }
  
  DetermineWhetherToPrintPage();
  // Nothing to do if the current page doesn't get printed OR rendering to
  // preview. For preview, the `CallPrintCallback` is called from within the
  // HTMLCanvasElement::HandlePrintCallback.
  if (!mPrintThisPage || !PresContext()->IsRootPaginatedDocument()) {
    *aDone = true;
    return NS_OK;
  }

  // If the canvasList is null, then generate it and start the render
  // process for all the canvas.
  if (!mCurrentCanvasListSetup) {
    mCurrentCanvasListSetup = true;
    GetPrintCanvasElementsInFrame(currentPage, &mCurrentCanvasList);

    if (mCurrentCanvasList.Length() != 0) {
      nsresult rv = NS_OK;

      // Begin printing of the document
      nsDeviceContext *dc = PresContext()->DeviceContext();
      PR_PL(("\n"));
      PR_PL(("***************** BeginPage *****************\n"));
      rv = dc->BeginPage();
      NS_ENSURE_SUCCESS(rv, rv);

      mCalledBeginPage = true;
      
      nsRefPtr<nsRenderingContext> renderingContext =
        dc->CreateRenderingContext();

      nsRefPtr<gfxASurface> renderingSurface =
          renderingContext->ThebesContext()->CurrentSurface();
      NS_ENSURE_TRUE(renderingSurface, NS_ERROR_OUT_OF_MEMORY);

      for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0 ; i--) {
        HTMLCanvasElement* canvas = mCurrentCanvasList[i];
        nsIntSize size = canvas->GetSize();

        nsRefPtr<gfxASurface> printSurface = renderingSurface->
           CreateSimilarSurface(
             gfxContentType::COLOR_ALPHA,
             size
           );

        if (!printSurface) {
          continue;
        }

        nsICanvasRenderingContextInternal* ctx = canvas->GetContextAtIndex(0);

        if (!ctx) {
          continue;
        }

          // Initialize the context with the new printSurface.
        ctx->InitializeWithSurface(nullptr, printSurface, size.width, size.height);

        // Start the rendering process.
        nsWeakFrame weakFrame = this;
        canvas->DispatchPrintCallback(aCallback);
        NS_ENSURE_STATE(weakFrame.IsAlive());
      }
    }
  }
  uint32_t doneCounter = 0;
  for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0 ; i--) {
    HTMLCanvasElement* canvas = mCurrentCanvasList[i];

    if (canvas->IsPrintCallbackDone()) {
      doneCounter++;
    }
  }
  // If all canvas have finished rendering, return true, otherwise false.
  *aDone = doneCounter == mCurrentCanvasList.Length();

  return NS_OK;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::ResetPrintCanvasList()
{
  for (int32_t i = mCurrentCanvasList.Length() - 1; i >= 0 ; i--) {
    HTMLCanvasElement* canvas = mCurrentCanvasList[i];
    canvas->ResetPrintCallback();
  }

  mCurrentCanvasList.Clear();
  mCurrentCanvasListSetup = false; 
  return NS_OK;
} 

NS_IMETHODIMP
nsSimplePageSequenceFrame::PrintNextPage()
{
  // Print each specified page
  // pageNum keeps track of the current page and what pages are printing
  //
  // printedPageNum keeps track of the current page number to be printed
  // Note: When print al the pages or a page range the printed page shows the
  // actual page number, when printing selection it prints the page number starting
  // with the first page of the selection. For example if the user has a 
  // selection that starts on page 2 and ends on page 3, the page numbers when
  // print are 1 and then two (which is different than printing a page range, where
  // the page numbers would have been 2 and then 3)

  nsIFrame* currentPage = GetCurrentPageFrame();
  if (!currentPage) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv = NS_OK;

  DetermineWhetherToPrintPage();

  if (mPrintThisPage) {
    // Begin printing of the document
    nsDeviceContext* dc = PresContext()->DeviceContext();

    // XXX This is temporary fix for printing more than one page of a selection
    // This does a poor man's "dump" pagination (see Bug 89353)
    // It has laid out as one long page and now we are just moving or view up/down 
    // one page at a time and printing the contents of what is exposed by the rect.
    // currently this does not work for IFrames
    // I will soon improve this to work with IFrames 
    bool    continuePrinting = true;
    nscoord width, height;
    width = PresContext()->GetPageSize().width;
    height = PresContext()->GetPageSize().height;
    height -= mMargin.top + mMargin.bottom;
    width  -= mMargin.left + mMargin.right;
    nscoord selectionY = height;
    nsIFrame* conFrame = currentPage->GetFirstPrincipalChild();
    if (mSelectionHeight >= 0) {
      conFrame->SetPosition(conFrame->GetPosition() + nsPoint(0, -mYSelOffset));
      nsContainerFrame::PositionChildViews(conFrame);
    }

    // cast the frame to be a page frame
    nsPageFrame * pf = static_cast<nsPageFrame*>(currentPage);
    pf->SetPageNumInfo(mPageNum, mTotalPages);
    pf->SetSharedPageData(mPageData);

    int32_t printedPageNum = 1;
    while (continuePrinting) {
      if (PresContext()->IsRootPaginatedDocument()) {
        if (!mCalledBeginPage) {
          PR_PL(("\n"));
          PR_PL(("***************** BeginPage *****************\n"));
          rv = dc->BeginPage();
          NS_ENSURE_SUCCESS(rv, rv);
        } else {
          mCalledBeginPage = false;
        }
      }

      PR_PL(("SeqFr::PrintNextPage -> %p PageNo: %d", pf, mPageNum));

      nsRefPtr<nsRenderingContext> renderingContext =
        dc->CreateRenderingContext();

      nsRect drawingRect(nsPoint(0, 0), currentPage->GetSize());
      nsRegion drawingRegion(drawingRect);
      nsLayoutUtils::PaintFrame(renderingContext, currentPage,
                                drawingRegion, NS_RGBA(0,0,0,0),
                                nsLayoutUtils::PAINT_SYNC_DECODE_IMAGES);

      if (mSelectionHeight >= 0 && selectionY < mSelectionHeight) {
        selectionY += height;
        printedPageNum++;
        pf->SetPageNumInfo(printedPageNum, mTotalPages);
        conFrame->SetPosition(conFrame->GetPosition() + nsPoint(0, -height));
        nsContainerFrame::PositionChildViews(conFrame);

        PR_PL(("***************** End Page (PrintNextPage) *****************\n"));
        rv = dc->EndPage();
        NS_ENSURE_SUCCESS(rv, rv);
      } else {
        continuePrinting = false;
      }
    }
  }
  return rv;
}

NS_IMETHODIMP
nsSimplePageSequenceFrame::DoPageEnd()
{
  nsresult rv = NS_OK;
  if (PresContext()->IsRootPaginatedDocument() && mPrintThisPage) {
    PR_PL(("***************** End Page (DoPageEnd) *****************\n"));
    rv = PresContext()->DeviceContext()->EndPage();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  ResetPrintCanvasList();

  mPageNum++;
  
  return rv;
}

static gfx::Matrix4x4
ComputePageSequenceTransform(nsIFrame* aFrame, float aAppUnitsPerPixel)
{
  float scale = aFrame->PresContext()->GetPrintPreviewScale();
  return gfx::Matrix4x4().Scale(scale, scale, 1);
}

void
nsSimplePageSequenceFrame::BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                            const nsRect&           aDirtyRect,
                                            const nsDisplayListSet& aLists)
{
  DisplayBorderBackgroundOutline(aBuilder, aLists);

  nsDisplayList content;

  {
    // Clear clip state while we construct the children of the
    // nsDisplayTransform, since they'll be in a different coordinate system.
    DisplayListClipState::AutoSaveRestore clipState(aBuilder);
    clipState.Clear();

    nsIFrame* child = GetFirstPrincipalChild();
    nsRect dirty = aDirtyRect;
    dirty.ScaleInverseRoundOut(PresContext()->GetPrintPreviewScale());

    while (child) {
      if (child->GetVisualOverflowRectRelativeToParent().Intersects(dirty)) {
        child->BuildDisplayListForStackingContext(aBuilder,
            dirty - child->GetPosition(), &content);
        aBuilder->ResetMarkedFramesForDisplayList();
      }
      child = child->GetNextSibling();
    }
  }

  content.AppendNewToTop(new (aBuilder)
      nsDisplayTransform(aBuilder, this, &content, content.GetVisibleRect(),
                         ::ComputePageSequenceTransform));

  aLists.Content()->AppendToTop(&content);
}

nsIAtom*
nsSimplePageSequenceFrame::GetType() const
{
  return nsGkAtoms::sequenceFrame; 
}

//------------------------------------------------------------------------------
void
nsSimplePageSequenceFrame::SetPageNumberFormat(const nsAString& aFormatStr, bool aForPageNumOnly)
{ 
  NS_ASSERTION(mPageData != nullptr, "mPageData string cannot be null!");

  if (aForPageNumOnly) {
    mPageData->mPageNumFormat = aFormatStr;
  } else {
    mPageData->mPageNumAndTotalsFormat = aFormatStr;
  }
}

//------------------------------------------------------------------------------
void
nsSimplePageSequenceFrame::SetDateTimeStr(const nsAString& aDateTimeStr)
{ 
  NS_ASSERTION(mPageData != nullptr, "mPageData string cannot be null!");

  mPageData->mDateTimeStr = aDateTimeStr;
}

//------------------------------------------------------------------------------
// For Shrink To Fit
//
// Return the percentage that the page needs to shrink to 
//
NS_IMETHODIMP
nsSimplePageSequenceFrame::GetSTFPercent(float& aSTFPercent)
{
  NS_ENSURE_TRUE(mPageData, NS_ERROR_UNEXPECTED);
  aSTFPercent = mPageData->mShrinkToFitRatio;
  return NS_OK;
}
