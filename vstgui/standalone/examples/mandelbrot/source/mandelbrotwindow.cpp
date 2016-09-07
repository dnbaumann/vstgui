#include "mandelbrotwindow.h"
#include "mandelbrot.h"
#include "mandelbrotview.h"
#include "vstgui/lib/cbitmap.h"
#include "vstgui/lib/ccolor.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/iscalefactorchangedlistener.h"
#include "vstgui/lib/iviewlistener.h"
#include "vstgui/standalone/helpers/value.h"
#include "vstgui/standalone/helpers/valuelistener.h"
#include "vstgui/standalone/iasync.h"
#include "vstgui/standalone/iuidescwindow.h"
#include "vstgui/uidescription/delegationcontroller.h"
#include "vstgui/uidescription/iuidescription.h"
#include "vstgui/uidescription/uiattributes.h"
#include <atomic>
#include <thread>
#include <cassert>

//------------------------------------------------------------------------
namespace Mandelbrot {

using namespace VSTGUI;
using namespace VSTGUI::Standalone;
using namespace VSTGUI::Standalone::Application;

//------------------------------------------------------------------------
inline CColor calculateColor (uint32_t iteration, double maxIterationInv)
{
	CColor color;
	const auto t = static_cast<double> (iteration) * maxIterationInv;
	color.red = static_cast<uint8_t> (9. * (1. - t) * t * t * t * 255.);
	color.green = static_cast<uint8_t> (15. * (1. - t) * (1. - t) * t * t * 255.);
	color.blue = static_cast<uint8_t> (8.5 * (1. - t) * (1. - t) * (1. - t) * t * 255.);
	return color;
}

//------------------------------------------------------------------------
inline std::function<uint32_t (CColor)> getColorToInt32 (IPlatformBitmapPixelAccess::PixelFormat f)
{
	switch (f)
	{
		case IPlatformBitmapPixelAccess::kARGB:
		{
			return [] (CColor color) {
				return (color.red << 8) | (color.green << 16) | (color.blue << 24) | (color.alpha);
			};
			break;
		}
		case IPlatformBitmapPixelAccess::kABGR:
		{
			return [] (CColor color) {
				return (color.blue << 8) | (color.green << 16) | (color.red << 24) | (color.alpha);
			};
			break;
		}
		case IPlatformBitmapPixelAccess::kRGBA:
		{
			return [] (CColor color) {
				return (color.red) | (color.green << 8) | (color.blue << 16) | (color.alpha << 24);
			};
			break;
		}
		case IPlatformBitmapPixelAccess::kBGRA:
		{
			return [] (CColor color) {
				return (color.blue) | (color.green << 8) | (color.red << 16) | (color.alpha << 24);
			};
			break;
		}
	}
}

//------------------------------------------------------------------------
template <typename ReadyCallback>
inline void calculateMandelbrotBitmap (Model model, SharedPointer<CBitmap> bitmap, CPoint size,
                                       uint32_t id, const std::atomic<uint32_t>& taskID,
                                       ReadyCallback readyCallback)
{
	if (auto pa = owned (CBitmapPixelAccess::create (bitmap)))
	{
		const auto numLinesPerTask =
		    static_cast<uint32_t> (size.y / (std::thread::hardware_concurrency () * 2));

		const auto maxIterationInv = 1. / model.getIterations ();

		auto pixelAccess = shared (pa->getPlatformBitmapPixelAccess ());
		auto colorToInt32 = getColorToInt32 (pixelAccess->getPixelFormat ());
		auto counter = std::make_shared<uint32_t> (0);
		for (auto y = 0u; y < static_cast<uint32_t> (size.y); y += numLinesPerTask)
		{
			++(*counter);
			auto task = [=, &taskID] () {
				for (auto i = 0u; i < numLinesPerTask; ++i)
				{
					if (y + i >= size.y || taskID != id)
						break;
					auto pixelPtr = reinterpret_cast<uint32_t*> (
					    pixelAccess->getAddress () + (y + i) * pixelAccess->getBytesPerRow ());
					calculateLine (y + i, size, model, [&] (auto x, auto iteration) {
						auto color = calculateColor (iteration, maxIterationInv);
						*pixelPtr = colorToInt32 (color);
						pixelPtr++;
					});
				}
				Async::perform (Async::Context::Main, [readyCallback, counter, bitmap, id] () {
					if (--(*counter) == 0)
					{
						readyCallback (id, bitmap);
					}
				});
			};
			Async::perform (Async::Context::Background, std::move (task));
		}
	}
}

//------------------------------------------------------------------------
struct ViewController : DelegationController,
                        IViewListenerAdapter,
                        IModelChangeListener,
                        IScaleFactorChangedListener,
						CBaseObject
{
	ViewController (IController* parent, Model::Ptr model)
	: DelegationController (parent), model (model)
	{
		model->registerListener (this);
	}
	~ViewController () noexcept { model->unregisterListener (this); }

	CView* createView (const UIAttributes& attributes, const IUIDescription* description) override
	{
		if (auto name = attributes.getAttributeValue (IUIDescription::kCustomViewName))
		{
			if (*name == "MandelbrotView")
			{
				mandelbrotView = new View ([&] (auto box) {
					auto min =
					    pixelToPoint (model->getMax (), model->getMin (),
					                  mandelbrotView->getViewSize ().getSize (), box.getTopLeft ());
					auto max = pixelToPoint (model->getMax (), model->getMin (),
					                         mandelbrotView->getViewSize ().getSize (),
					                         box.getBottomRight ());
					model->setMinMax (min, max);
				});
				mandelbrotView->registerViewListener (this);
				return mandelbrotView;
			}
		}
		return controller->createView (attributes, description);
	}

	void viewSizeChanged (CView* view, const CRect& oldSize) override { updateMandelbrot (); }
	void viewAttached (CView* view) override
	{
		if (auto frame = view->getFrame ())
		{
			frame->registerScaleFactorChangedListeneer (this);
			scaleFactor = frame->getScaleFactor ();
			updateMandelbrot ();
		}
	}
	void viewRemoved (CView* view) override
	{
		if (auto frame = view->getFrame ())
		{
			frame->unregisterScaleFactorChangedListeneer (this);
		}
	}
	void viewWillDelete (CView* view) override
	{
		assert (mandelbrotView == view);
		++taskID; // cancel background calculation
		mandelbrotView->unregisterViewListener (this);
		mandelbrotView = nullptr;
	}

	void onScaleFactorChanged (CFrame* frame, double newScaleFactor) override
	{
		if (scaleFactor != newScaleFactor)
		{
			scaleFactor = newScaleFactor;
			updateMandelbrot ();
		}
	}

	void modelChanged (const Model& model) override { updateMandelbrot (); }

	void updateMandelbrot ()
	{
		CPoint size = mandelbrotView->getViewSize ().getSize ();
		size.x *= scaleFactor;
		size.y *= scaleFactor;
		auto bitmap = owned (new CBitmap (size.x, size.y));
		bitmap->getPlatformBitmap ()->setScaleFactor (scaleFactor);
		auto id = ++taskID;
		auto This = shared (this);
		calculateMandelbrotBitmap (*model.get (), bitmap, size, id, taskID,
		                           [This] (uint32_t id, SharedPointer<CBitmap> bitmap) {
			                           if (id == This->taskID && This->mandelbrotView)
				                           This->mandelbrotView->setBackground (bitmap);
			                       });
	}

	Model::Ptr model;
	CView* mandelbrotView {nullptr};
	double scaleFactor {1.};
	std::atomic<uint32_t> taskID {0};
};

//------------------------------------------------------------------------
struct Customization : UIDesc::ICustomization
{
	Customization (Model::Ptr model) : model (model) {}

	IController* createController (const UTF8StringView& name, IController* parent,
	                               const IUIDescription* uiDesc) override
	{
		return new ViewController (parent, model);
	}
	Model::Ptr model;
};

//------------------------------------------------------------------------
struct ModelBinding : UIDesc::IModelBinding, IModelChangeListener, ValueListenerAdapter
{
	using Ptr = std::shared_ptr<ModelBinding>;

	ModelBinding (Model::Ptr model) : model (model)
	{
		if (auto sv = maxIterations->dynamicCast<IStepValue> ())
			maxIterations->performEdit (sv->stepToValue (model->getIterations ()));

		values.emplace_back (maxIterations);

		maxIterations->registerListener (this);
	}

	const ValueList& getValues () const override { return values; }

	void modelChanged (const Model& model) override
	{
		if (auto sv = maxIterations->dynamicCast<IStepValue> ())
		{
			maxIterations->beginEdit ();
			maxIterations->performEdit (sv->stepToValue (model.getIterations ()));
			maxIterations->endEdit ();
		}
	}
	void onPerformEdit (IValue& value, IValue::Type newValue) override
	{
		if (&value == maxIterations.get ())
		{
			if (auto sv = maxIterations->dynamicCast<IStepValue> ())
			{
				model->setIterations (sv->valueToStep (value.getValue ()));
			}
		}
	}

	ValuePtr maxIterations {Value::makeStepValue ("max interations", 1024)};
	ValueList values;

	Model::Ptr model;
};

//------------------------------------------------------------------------
VSTGUI::Standalone::WindowPtr makeMandelbrotWindow ()
{
	auto model = std::make_shared<Model> ();
	auto modelBinding = std::make_shared<ModelBinding> (model);
	UIDesc::Config config;
	config.uiDescFileName = "Window.uidesc";
	config.viewName = "Window";
	config.modelBinding = modelBinding;
	config.customization = std::make_shared<Customization> (model);
	config.windowConfig.title = "Mandelbrot";
	config.windowConfig.autoSaveFrameName = "Mandelbrot";
	config.windowConfig.style.border ().close ().size ().centered ();
	return UIDesc::makeWindow (config);
}

//------------------------------------------------------------------------
} // Mandelbrot
