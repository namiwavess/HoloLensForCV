//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the Microsoft Public License.
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#include "pch.h"
#include <cmath>
#include <MemoryBuffer.h>
#include "FrameRenderer.h"

using namespace SensorStreaming;

using namespace concurrency;
using namespace Platform;
using namespace Microsoft::WRL;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Capture::Frames;
using namespace Windows::Media::MediaProperties;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media::Imaging;

#pragma region Low-level operations on reference pointers

// InterlockedExchange for reference pointer types.
template<typename T, typename U>
T^ InterlockedExchangeRefPointer(T^* target, U value)
{
    static_assert(sizeof(T^) == sizeof(void*), "InterlockedExchangePointer is the wrong size");
    T^ exchange = value;
    void** rawExchange = reinterpret_cast<void**>(&exchange);
    void** rawTarget = reinterpret_cast<void**>(target);
    *rawExchange = static_cast<IInspectable*>(InterlockedExchangePointer(rawTarget, *rawExchange));
    return exchange;
}

// Convert a reference pointer to a specific ComPtr.
template<typename T>
Microsoft::WRL::ComPtr<T> AsComPtr(Platform::Object^ object)
{
    Microsoft::WRL::ComPtr<T> p;
    reinterpret_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&p));
    return p;
}
#pragma endregion

// Structure used to access colors stored in 8-bit BGRA format.
#pragma pack (push, 1)
struct ColorBGRA
{
    byte B, G, R, A;
};
#pragma pack(pop)

// Colors to map values to based on intensity.
static constexpr std::array<ColorBGRA, 9> colorRamp = {
    ColorBGRA{ 0xFF, 0x7F, 0x00, 0x00 },
    ColorBGRA{ 0xFF, 0xFF, 0x00, 0x00 },
    ColorBGRA{ 0xFF, 0xFF, 0x7F, 0x00 },
    ColorBGRA{ 0xFF, 0xFF, 0xFF, 0x00 },
    ColorBGRA{ 0xFF, 0x7F, 0xFF, 0x7F },
    ColorBGRA{ 0xFF, 0x00, 0xFF, 0xFF },
    ColorBGRA{ 0xFF, 0x00, 0x7F, 0xFF },
    ColorBGRA{ 0xFF, 0x00, 0x00, 0xFF },
    ColorBGRA{ 0xFF, 0x00, 0x00, 0x7F }
};

static ColorBGRA ColorRampInterpolation(float value)
{
    static_assert(colorRamp.size() >= 2, "colorRamp table is too small");

    // Map value to surrounding indexes on the color ramp.
    size_t rampSteps = colorRamp.size() - 1;
    float scaled = value * rampSteps;
    int integer = static_cast<int>(scaled);
    int index = min(static_cast<size_t>(max(0, integer)), rampSteps - 1);
    const ColorBGRA& prev = colorRamp[index];
    const ColorBGRA& next = colorRamp[index + 1];
    // 8bitのBGRAフォーマット
    // Set color based on a ratio of how closely it matches the surrounding colors.
    UINT32 alpha = static_cast<UINT32>((scaled - integer) * 255);
    UINT32 beta = 255 - alpha;
    return {
        static_cast<byte>((prev.A * beta + next.A * alpha) / 255), // Alpha
        static_cast<byte>((prev.R * beta + next.R * alpha) / 255), // Red
        static_cast<byte>((prev.G * beta + next.G * alpha) / 255), // Green
        static_cast<byte>((prev.B * beta + next.B * alpha) / 255)  // Blue
    };
}

// Initializes pseudo-color look up table for depth pixels
static ColorBGRA GeneratePseudoColorLookupTable(UINT32 index, UINT32 size)
{
    return ColorRampInterpolation(static_cast<float>(index) / static_cast<float>(size));
}

// Initializes the pseudo-color look up table for infrared pixels
static ColorBGRA GenerateInfraredRampLookupTable(UINT32 index, UINT32 size)
{
    const float value = static_cast<float>(index) / static_cast<float>(size);

    // Adjust to increase color change between lower values in infrared images.
    const float alpha = powf(1 - value, 12);

    return ColorRampInterpolation(alpha);
}

static LookupTable<ColorBGRA, 1024> colorLookupTable(GeneratePseudoColorLookupTable);
static LookupTable<ColorBGRA, 1024> infraredLookupTable(GenerateInfraredRampLookupTable);

static ColorBGRA PseudoColor(float value)
{
    return colorLookupTable.GetValue(value);
}

static ColorBGRA InfraredColor(float value)
{
    return infraredLookupTable.GetValue(value);
}

// Maps each pixel in a scanline from a 16 bit depth value to a pseudo-color pixel.
static void PseudoColorForDepth(int pixelWidth, byte* inputRowBytes, byte* outputRowBytes, float depthScale, float minReliableDepth, float maxReliableDepth)
{
    // Visualize space in front of your desktop, in meters.
    const float rangeReciprocal = 1.0f / (maxReliableDepth - minReliableDepth);

    UINT16* inputRow = reinterpret_cast<UINT16*>(inputRowBytes);
    ColorBGRA* outputRow = reinterpret_cast<ColorBGRA*>(outputRowBytes);
    for (int x = 0; x < pixelWidth; x++)
    {
        // Map invalid depth values to transparent pixels.
        // This happens when depth information cannot be calculated, e.g. when objects are too close.
        if (inputRow[x] == 0 || inputRow[x] > 4000)
        {
            outputRow[x] = { 0xFF, 0x00, 0x00, 0x7F };
        }
        else
        {
            const float depth = static_cast<float>(inputRow[x]) * depthScale;
            const float alpha = (depth - minReliableDepth) * rangeReciprocal;
            outputRow[x] = PseudoColor(alpha);
        }
    }
}

// Maps each pixel in a scanline from a 16 bit infrared value to a pseudo-color pixel.
static void PseudoColorFor16BitInfrared(int pixelWidth, byte* inputRowBytes, byte* outputRowBytes)
{
    UINT16* inputRow = reinterpret_cast<UINT16*>(inputRowBytes);
    ColorBGRA* outputRow = reinterpret_cast<ColorBGRA*>(outputRowBytes);
    const float rangeReciprocal = 1.0f / static_cast<float>(UINT16_MAX);
    for (int x = 0; x < pixelWidth; x++)
    {
        if (inputRow[x] == 0)
        {
            outputRow[x] = { 0xFF, 0x00, 0x00, 0x7F };
        }
        else
        {
            outputRow[x] = InfraredColor(inputRow[x] * rangeReciprocal);
        }
    }
}

// Maps each pixel in a scanline from a 8 bit infrared value to a pseudo-color pixel.
static void PseudoColorFor8BitInfrared(int pixelWidth, byte* inputRowBytes, byte* outputRowBytes)
{
    ColorBGRA* outputRow = reinterpret_cast<ColorBGRA*>(outputRowBytes);
    const float rangeReciprocal = 1.0f / static_cast<float>(UINT8_MAX);
    for (int x = 0; x < pixelWidth; x++)
    {
        if (inputRowBytes[x] == 0)
        {
            outputRow[x] = { 0xFF, 0x00, 0x00, 0x7F };
        }
        else
        {
            outputRow[x] = InfraredColor(inputRowBytes[x] * rangeReciprocal);
        }
    }
}

FrameRenderer::FrameRenderer(Image^ imageElement)
{
    m_imageElement = imageElement;
    m_imageElement->Source = ref new SoftwareBitmapSource();
}

void FrameRenderer::SetSensorName(Platform::String^ sensorName)
{
    m_sensorName = sensorName;
}

void FrameRenderer::ProcessFrame(Windows::Media::Capture::Frames::MediaFrameReference^ frame)
{
    if (frame == nullptr)
    {
        return;
    }

    //
    // Allow a few frames to be buffered...
    //
    if (InterlockedIncrement(&m_numberOfTasksScheduled) > c_maxNumberOfTasksScheduled)
    {
        InterlockedDecrement(&m_numberOfTasksScheduled);

        return;
    }

    SoftwareBitmap^ softwareBitmap = ConvertToDisplayableImage(frame->VideoMediaFrame);
    if (!softwareBitmap)
    {
        InterlockedDecrement(&m_numberOfTasksScheduled);

        return;
    }

    m_imageElement->Dispatcher->RunAsync(
        Windows::UI::Core::CoreDispatcherPriority::Normal,
        ref new Windows::UI::Core::DispatchedHandler(
            [this, softwareBitmap]()
    {
        InterlockedDecrement(&m_numberOfTasksScheduled);

        //
        // ..but don't let too many copies of this task run at the same time:
        //
        if (InterlockedIncrement(&m_numberOfTasksRunning) > c_maxNumberOfTasksRunning)
        {
            InterlockedDecrement(&m_numberOfTasksRunning);

            return;
        }

        Windows::UI::Xaml::Media::Imaging::SoftwareBitmapSource^ imageSource =
            ref new Windows::UI::Xaml::Media::Imaging::SoftwareBitmapSource();

        concurrency::create_task(
            imageSource->SetBitmapAsync(softwareBitmap)
        ).then([this, imageSource]()
        {
            m_imageElement->Source = imageSource;

            InterlockedDecrement(&m_numberOfTasksRunning);

        }, concurrency::task_continuation_context::use_current());
    }));
}

String^ FrameRenderer::GetSubtypeForFrameReader(MediaFrameSourceKind kind, MediaFrameFormat^ format)
{
    // Note that media encoding subtypes may differ in case.
    // https://docs.microsoft.com/en-us/uwp/api/Windows.Media.MediaProperties.MediaEncodingSubtypes

    String^ subtype = format->Subtype;
    switch (kind)
    {
        // For color sources, we accept anything and request that it be converted to Bgra8.
    case MediaFrameSourceKind::Color:
        return MediaEncodingSubtypes::Bgra8;

        // The only depth format we can render is D16.
    case MediaFrameSourceKind::Depth:
        return CompareStringOrdinal(subtype->Data(), -1, L"D16", -1, TRUE) == CSTR_EQUAL ? subtype : nullptr;

        // The only infrared formats we can render are L8 and L16.
    case MediaFrameSourceKind::Infrared:
        return (CompareStringOrdinal(subtype->Data(), -1, L"L8", -1, TRUE) == CSTR_EQUAL ||
            CompareStringOrdinal(subtype->Data(), -1, L"D16", -1, TRUE) == CSTR_EQUAL) ? subtype : nullptr;

        // No other source kinds are supported by this class.
    default:
        return nullptr;
    }
}

SoftwareBitmap^ FrameRenderer::ConvertToDisplayableImage(VideoMediaFrame^ inputFrame)
{
    if (inputFrame == nullptr)
    {
        return nullptr;
    }

    try
    {
        SoftwareBitmap^ inputBitmap =
            inputFrame->SoftwareBitmap;

        switch (inputFrame->FrameReference->SourceKind)
        {
        case MediaFrameSourceKind::Color:
            // XAML requires Bgra8 with premultiplied alpha.
            // We requested Bgra8 from the MediaFrameReader, so all that's
            // left is fixing the alpha channel if necessary.
            if (inputBitmap->BitmapPixelFormat != BitmapPixelFormat::Bgra8)
            {
                OutputDebugStringW(L"Color format should have been Bgra8.\r\n");
            }
            else if (inputBitmap->PixelWidth == 640 / 4)
            {
                return TransformVlcBitmap(inputBitmap);
            }
            else
            {
#if 0
                if (inputBitmap->BitmapAlphaMode == BitmapAlphaMode::Premultiplied)
                {
                    // Already in the correct format.
                    return SoftwareBitmap::Copy(inputBitmap);
                }
                else
                {
                    // Convert to premultiplied alpha.
                    return SoftwareBitmap::Convert(inputBitmap, BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied);
                }
#else
                return DeepCopyBitmap(inputBitmap);
#endif
            }
            return nullptr;

        case MediaFrameSourceKind::Depth:
            // We requested D16 from the MediaFrameReader, so the frame should
            // be in Gray16 format.

            if (inputBitmap->BitmapPixelFormat == BitmapPixelFormat::Gray16)
            {
                using namespace std::placeholders
                
                // 深度距離
                // Use a special pseudo color to render 16 bits depth frame.
                // Since we must scale the output appropriately we use std::bind to
                // create a function that takes the depth scale as input but also matches
                // the required signature.
                const float depthScale = 1.0f / 1000.0f;
                float minReliableDepth, maxReliableDepth;

                if (m_sensorName == L"Long Throw ToF Depth")
                {
                    minReliableDepth = 0.5f;
                    maxReliableDepth = 4.0f;
                }
                else
                {
                    minReliableDepth = 0.2f;
                    maxReliableDepth = 1.0f;
                }

                return TransformBitmap(inputBitmap, std::bind(&PseudoColorForDepth, _1, _2, _3, depthScale, minReliableDepth, maxReliableDepth));
            }
            else
            {
                OutputDebugStringW(L"Depth format in unexpected format.\r\n");
            }
            return nullptr;

        case MediaFrameSourceKind::Infrared:
            // We requested L8 or L16 from the MediaFrameReader, so the frame should
            // be in Gray8 or Gray16 format. 
            switch (inputBitmap->BitmapPixelFormat)
            {
            case BitmapPixelFormat::Gray8:
                // Use pseudo color to render 8 bits frames.
                return TransformBitmap(inputBitmap, PseudoColorFor8BitInfrared);

            case BitmapPixelFormat::Gray16:
                // Use pseudo color to render 16 bits frames.
                return TransformBitmap(inputBitmap, PseudoColorFor16BitInfrared);

            default:
                OutputDebugStringW(L"Infrared format should have been Gray8 or Gray16.\r\n");
                return nullptr;
            }
        }
    }
    catch (Platform::Exception^ exception)
    {
        OutputDebugString(L"FrameRenderer::ConvertToDisplayableImage: exception thrown: ");
        OutputDebugString(exception->Message->Data());
        OutputDebugString(L"\n");
    }

    return nullptr;
}

SoftwareBitmap^ FrameRenderer::TransformVlcBitmap(SoftwareBitmap^ inputBitmap)
{
    // XAML Image control only supports premultiplied Bgra8 format.
    SoftwareBitmap^ outputBitmap = ref new SoftwareBitmap(
        BitmapPixelFormat::Bgra8,
        480 / 2,
        640 / 2,
        BitmapAlphaMode::Premultiplied);

    BitmapBuffer^ input = inputBitmap->LockBuffer(BitmapBufferAccessMode::Read);
    BitmapBuffer^ output = outputBitmap->LockBuffer(BitmapBufferAccessMode::Write);

    int inputStride = input->GetPlaneDescription(0).Stride;
    int outputStride = output->GetPlaneDescription(0).Stride;

    IMemoryBufferReference^ inputReference = input->CreateReference();
    IMemoryBufferReference^ outputReference = output->CreateReference();

    // Get input and output byte access buffers.
    byte* inputBytes;
    UINT32 inputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(inputReference)->GetBuffer(&inputBytes, &inputCapacity);

    byte* outputBytes;
    UINT32 outputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(outputReference)->GetBuffer(&outputBytes, &outputCapacity);

    for (int y = 0; y < 480; y += 2)
    {
        byte* inputRowBytes = inputBytes + y * 640;
        uint8_t* inputRow = reinterpret_cast<uint8_t*>(inputRowBytes);

        for (int x = 0; x < 640; x += 2)
        {
            const byte input =
                static_cast<byte>(
                    (static_cast<uint32_t>(inputRow[x]) +
                     static_cast<uint32_t>(inputRow[x + 1]) +
                     static_cast<uint32_t>(inputRow[x + 640]) +
                     static_cast<uint32_t>(inputRow[x + 641])) >> 2);

            byte* outputRowBytes = outputBytes + (x * outputStride >> 1);
            ColorBGRA* outputRow = reinterpret_cast<ColorBGRA*>(outputRowBytes);
            auto& output = outputRow[(480 / 2 - 1) - (y >> 1)];

            output.B = input;
            output.G = input;
            output.R = input;
            output.A = 255;
        }
    }

    // Close objects that need closing.
    delete outputReference;
    delete inputReference;
    delete output;
    delete input;

    return outputBitmap;
}

SoftwareBitmap^ FrameRenderer::DeepCopyBitmap(SoftwareBitmap^ inputBitmap)
{
    // XAML Image control only supports premultiplied Bgra8 format.
    SoftwareBitmap^ outputBitmap = ref new SoftwareBitmap(
        BitmapPixelFormat::Bgra8,
        inputBitmap->PixelWidth,
        inputBitmap->PixelHeight,
        BitmapAlphaMode::Premultiplied);

    BitmapBuffer^ input = inputBitmap->LockBuffer(BitmapBufferAccessMode::Read);
    BitmapBuffer^ output = outputBitmap->LockBuffer(BitmapBufferAccessMode::Write);

    // Get stride values to calculate buffer position for a given pixel x and y position.
    int inputStride = input->GetPlaneDescription(0).Stride;
    int outputStride = output->GetPlaneDescription(0).Stride;

    int pixelWidth = inputBitmap->PixelWidth;
    int pixelHeight = inputBitmap->PixelHeight;

    IMemoryBufferReference^ inputReference = input->CreateReference();
    IMemoryBufferReference^ outputReference = output->CreateReference();

    // Get input and output byte access buffers.
    byte* inputBytes;
    UINT32 inputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(inputReference)->GetBuffer(&inputBytes, &inputCapacity);

    byte* outputBytes;
    UINT32 outputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(outputReference)->GetBuffer(&outputBytes, &outputCapacity);

    // Iterate over all pixels, and store the converted value.
    for (int y = 0; y < pixelHeight; y++)
    {
        byte* inputRowBytes = inputBytes + y * inputStride;
        byte* outputRowBytes = outputBytes + y * outputStride;

        for (int x = 0; x < inputStride; ++x)
        {
            outputRowBytes[x] = inputRowBytes[x];
        }
    }

    // Close objects that need closing.
    delete outputReference;
    delete inputReference;
    delete output;
    delete input;

    return outputBitmap;
}

SoftwareBitmap^ FrameRenderer::TransformBitmap(SoftwareBitmap^ inputBitmap, TransformScanline pixelTransformation)
{
    // XAML Image control only supports premultiplied Bgra8 format.
    SoftwareBitmap^ outputBitmap = ref new SoftwareBitmap(
        BitmapPixelFormat::Bgra8,
        inputBitmap->PixelWidth,
        inputBitmap->PixelHeight,
        BitmapAlphaMode::Premultiplied);

    BitmapBuffer^ input = inputBitmap->LockBuffer(BitmapBufferAccessMode::Read);
    BitmapBuffer^ output = outputBitmap->LockBuffer(BitmapBufferAccessMode::Write);

    // Get stride values to calculate buffer position for a given pixel x and y position.
    int inputStride = input->GetPlaneDescription(0).Stride;
    int outputStride = output->GetPlaneDescription(0).Stride;

    int pixelWidth = inputBitmap->PixelWidth;
    int pixelHeight = inputBitmap->PixelHeight;

    IMemoryBufferReference^ inputReference = input->CreateReference();
    IMemoryBufferReference^ outputReference = output->CreateReference();

    // Get input and output byte access buffers.
    byte* inputBytes;
    UINT32 inputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(inputReference)->GetBuffer(&inputBytes, &inputCapacity);

    byte* outputBytes;
    UINT32 outputCapacity;
    AsComPtr<IMemoryBufferByteAccess>(outputReference)->GetBuffer(&outputBytes, &outputCapacity);

    // Iterate over all pixels, and store the converted value.
    for (int y = 0; y < pixelHeight; y++)
    {
        byte* inputRowBytes = inputBytes + y * inputStride;
        byte* outputRowBytes = outputBytes + y * outputStride;

        pixelTransformation(pixelWidth, inputRowBytes, outputRowBytes);
    }

    // Close objects that need closing.
    delete outputReference;
    delete inputReference;
    delete output;
    delete input;

    return outputBitmap;
}
