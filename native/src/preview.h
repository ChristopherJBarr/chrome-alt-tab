#pragma once
#include <windows.h>
#include <winrt/base.h>
#include <shlwapi.h>
#include <vector>
#include <wincrypt.h>
#include <wincodec.h>
#include <algorithm>

namespace bridge
{
	struct PreviewPixels { UINT width = 0, height = 0; std::vector<BYTE> pixels; };
	inline PreviewPixels decodePreview(const winrt::hstring& encoded)
	{
		if (encoded.empty() || encoded.size() > 174764) { throw winrt::hresult_invalid_argument(L"Preview exceeds 128 KiB"); }
		DWORD size{};
		constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
		winrt::check_bool(CryptStringToBinaryW(encoded.c_str(), encoded.size(), flags, nullptr, &size, nullptr, nullptr));
		if (!size || size > 128 * 1024) { throw winrt::hresult_invalid_argument(L"Invalid preview size"); }
		std::vector<BYTE> bytes(size);
		winrt::check_bool(CryptStringToBinaryW(encoded.c_str(), encoded.size(), flags, bytes.data(), &size, nullptr, nullptr));
		winrt::com_ptr<IStream> stream;
		stream.attach(SHCreateMemStream(bytes.data(), size));
		if (!stream) { throw winrt::hresult_error(E_OUTOFMEMORY); }
		auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
		winrt::com_ptr<IWICBitmapDecoder> decoder;
		winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
		GUID format{};
		winrt::check_hresult(decoder->GetContainerFormat(&format));
		if (format != GUID_ContainerFormatJpeg) { throw winrt::hresult_invalid_argument(L"Preview must be JPEG"); }
		winrt::com_ptr<IWICBitmapFrameDecode> frame;
		winrt::check_hresult(decoder->GetFrame(0, frame.put()));
		UINT width{}, height{};
		winrt::check_hresult(frame->GetSize(&width, &height));
		if (!width || !height || width > 640 || height > 400) { throw winrt::hresult_invalid_argument(L"Preview dimensions exceed 640 x 400"); }
		// Decode all pixels now, so truncated/corrupt images never reach the shell.
		winrt::com_ptr<IWICFormatConverter> converter;
		winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
		winrt::check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
			WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
		std::vector<BYTE> pixels(width * height * 4);
		winrt::check_hresult(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
		return {width, height, std::move(pixels)};
	}
}
