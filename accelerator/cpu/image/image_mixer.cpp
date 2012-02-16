/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#include "../../stdafx.h"

#include "image_mixer.h"

#include "../util/data_frame.h"
#include "../util/xmm.h"

#include <common/assert.h>
#include <common/gl/gl_check.h>
#include <common/concurrency/async.h>
#include <common/memory/memcpy.h>

#include <core/frame/data_frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>
#include <core/video_format.h>

#include <modules/ffmpeg/producer/util/util.h>

#include <asmlib.h>

#include <gl/glew.h>

#include <tbb/cache_aligned_allocator.h>
#include <tbb/parallel_for_each.h>
#include <tbb/concurrent_queue.h>

#include <boost/assign.hpp>
#include <boost/foreach.hpp>
#include <boost/range.hpp>
#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/thread/future.hpp>

#include <algorithm>
#include <stdint.h>
#include <vector>

#if defined(_MSC_VER)
#pragma warning (push)
#pragma warning (disable : 4244)
#endif
extern "C" 
{
	#include <libswscale/swscale.h>
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
}
#if defined(_MSC_VER)
#pragma warning (pop)
#endif

namespace caspar { namespace accelerator { namespace cpu {
		
struct item
{
	core::pixel_format_desc			pix_desc;
	std::array<const uint8_t*, 4>	data;
	core::image_transform			transform;

	item()
		: pix_desc(core::pixel_format::invalid)
	{
		data.fill(0);
	}
};

bool operator==(const item& lhs, const item& rhs)
{
	return lhs.data == rhs.data && lhs.transform == rhs.transform;
}

bool operator!=(const item& lhs, const item& rhs)
{
	return !(lhs == rhs);
}
	
inline xmm::s8_x blend(xmm::s8_x d, xmm::s8_x s)
{	
	using namespace xmm;
		
	// T(S, D) = S * D[A] + 0x80
	auto aaaa   = s8_x::shuffle(d, s8_x(15, 15, 15, 15, 11, 11, 11, 11, 7, 7, 7, 7, 3, 3, 3, 3));
	d			= s8_x(u8_x::min(u8_x(d), u8_x(aaaa))); // overflow guard

	auto xaxa	= s16_x(aaaa) >> 8;		
			      
	auto t1		= s16_x::multiply_low(s16_x(s) & 0x00FF, xaxa) + 0x80;    
	auto t2		= s16_x::multiply_low(s16_x(s) >> 8    , xaxa) + 0x80;
		
	// C(S, D) = S + D - (((T >> 8) + T) >> 8);
	auto xyxy	= s8_x(((t1 >> 8) + t1) >> 8);      
	auto yxyx	= s8_x((t2 >> 8) + t2);    
	auto argb   = s8_x::blend(xyxy, yxyx, s8_x(-1, 0, -1, 0));

	return s8_x(s) + (d - argb);
}
	
template<typename temporal, typename alignment>
static void kernel(uint8_t* dest, const uint8_t* source, size_t count)
{			
	using namespace xmm;

	for(auto n = 0; n < count; n += 32)    
	{
		auto s0 = s8_x::load<temporal_tag, alignment>(dest+n+0);
		auto s1 = s8_x::load<temporal_tag, alignment>(dest+n+16);

		auto d0 = s8_x::load<temporal_tag, alignment>(source+n+0);
		auto d1 = s8_x::load<temporal_tag, alignment>(source+n+16);
		
		auto argb0 = blend(d0, s0);
		auto argb1 = blend(d1, s1);

		s8_x::store<temporal, alignment>(argb0, dest+n+0 );
		s8_x::store<temporal, alignment>(argb1, dest+n+16);
	} 
}

template<typename temporal>
static void kernel(uint8_t* dest, const uint8_t* source, size_t count)
{			
	using namespace xmm;

	if(reinterpret_cast<int>(dest) % 16 != 0 || reinterpret_cast<int>(source) % 16 != 0)
		kernel<temporal_tag, unaligned_tag>(dest, source, count);
	else
		kernel<temporal_tag, aligned_tag>(dest, source, count);
}

class image_renderer
{
	std::pair<std::vector<item>, boost::shared_future<boost::iterator_range<const uint8_t*>>>		last_image_;
	tbb::concurrent_unordered_map<int, tbb::concurrent_bounded_queue<std::shared_ptr<SwsContext>>>	sws_contexts_;
	tbb::concurrent_bounded_queue<spl::shared_ptr<host_buffer>>										temp_buffers_;
public:	
	boost::shared_future<boost::iterator_range<const uint8_t*>> operator()(std::vector<item> items, const core::video_format_desc& format_desc)
	{	
		if(last_image_.first == items && last_image_.second.has_value())
			return last_image_.second;

		auto image	= render(items, format_desc);
		last_image_ = std::make_pair(std::move(items), image);
		return image;
	}

private:
	boost::shared_future<boost::iterator_range<const uint8_t*>> render(std::vector<item> items, const core::video_format_desc& format_desc)
	{
		convert(items, format_desc.width, format_desc.height);		
		
		auto result = spl::make_shared<host_buffer>(format_desc.size, 0);
		if(format_desc.field_mode != core::field_mode::progressive)
		{			
			draw(items, result->data(), format_desc.width, format_desc.height, core::field_mode::upper);
			draw(items, result->data(), format_desc.width, format_desc.height, core::field_mode::lower);
		}
		else
		{
			draw(items, result->data(), format_desc.width, format_desc.height,  core::field_mode::progressive);
		}

		temp_buffers_.clear();
		
		return async(launch_policy::deferred, [=]
		{
			return boost::iterator_range<const uint8_t*>(result->data(), result->data() + format_desc.size);
		});		
	}

	void draw(std::vector<item> items, uint8_t* dest, int width, int height, core::field_mode field_mode)
	{		
		BOOST_FOREACH(auto& item, items)
			item.transform.field_mode &= field_mode;
		
		// Remove empty items.
		boost::range::remove_erase_if(items, [&](const item& item)
		{
			return item.transform.field_mode == core::field_mode::empty;
		});
		
		// Remove first field stills.
		boost::range::remove_erase_if(items, [&](const item& item)
		{
			return item.transform.is_still && item.transform.field_mode == field_mode; // only us last field for stills.
		});

		if(items.empty())
			return;
		
		auto start = field_mode == core::field_mode::lower ? 1 : 0;
		auto step  = field_mode == core::field_mode::progressive ? 1 : 2;
		
		// TODO: Add support for fill translations.
		// TODO: Add support for mask rect.
		// TODO: Add support for opacity.
		// TODO: Add support for mix transition.
		// TODO: Add support for push transition.
		// TODO: Add support for wipe transition.
		// TODO: Add support for slide transition.
		tbb::parallel_for(tbb::blocked_range<int>(0, height/step), [&](const tbb::blocked_range<int>& r)
		{
			for(auto i = r.begin(); i != r.end(); ++i)
			{
				auto y = i*step+start;

				for(std::size_t n = 0; n < items.size()-1; ++n)
					kernel<xmm::temporal_tag>(dest + y*width*4, items[n].data.at(0) + y*width*4, width*4);
				
				std::size_t n = items.size()-1;				
				kernel<xmm::nontemporal_tag>(dest + y*width*4, items[n].data.at(0) + y*width*4, width*4);
			}

			_mm_mfence();
		});
	}
		
	void convert(std::vector<item>& source_items, int width, int height)
	{
		std::set<std::array<const uint8_t*, 4>> buffers;

		BOOST_FOREACH(auto& item, source_items)
			buffers.insert(item.data);
		
		auto dest_items = source_items;

		tbb::parallel_for_each(buffers.begin(), buffers.end(), [&](const std::array<const uint8_t*, 4>& data)
		{			
			auto pix_desc = std::find_if(source_items.begin(), source_items.end(), [&](const item& item){return item.data == data;})->pix_desc;

			if(pix_desc.format == core::pixel_format::bgra && 
				pix_desc.planes.at(0).width == width &&
				pix_desc.planes.at(0).height == height)
				return;

			std::array<uint8_t*, 4> data2 = {};
			for(std::size_t n = 0; n < data.size(); ++n)
				data2.at(n) = const_cast<uint8_t*>(data[n]);

			auto input_av_frame = ffmpeg::make_av_frame(data2, pix_desc);
								
			int key = ((input_av_frame->width << 22) & 0xFFC00000) | ((input_av_frame->height << 6) & 0x003FC000) | ((input_av_frame->format << 7) & 0x00007F00);
						
			auto& pool = sws_contexts_[key];

			std::shared_ptr<SwsContext> sws_context;
			if(!pool.try_pop(sws_context))
			{
				double param;
				sws_context.reset(sws_getContext(input_av_frame->width, input_av_frame->height, static_cast<PixelFormat>(input_av_frame->format), width, height, PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, &param), sws_freeContext);
			}
			
			if(!sws_context)				
				BOOST_THROW_EXCEPTION(operation_failed() << msg_info("Could not create software scaling context.") << boost::errinfo_api_function("sws_getContext"));				
		
			auto dest_frame = spl::make_shared<host_buffer>(width*height*4);
			temp_buffers_.push(dest_frame);

			{
				spl::shared_ptr<AVFrame> dest_av_frame(avcodec_alloc_frame(), av_free);	
				avcodec_get_frame_defaults(dest_av_frame.get());			
				avpicture_fill(reinterpret_cast<AVPicture*>(dest_av_frame.get()), dest_frame->data(), PIX_FMT_BGRA, width, height);
				
				sws_scale(sws_context.get(), input_av_frame->data, input_av_frame->linesize, 0, input_av_frame->height, dest_av_frame->data, dest_av_frame->linesize);				
				pool.push(sws_context);
			}
					
			for(std::size_t n = 0; n < source_items.size(); ++n)
			{
				if(source_items[n].data == data)
				{
					dest_items[n].data.assign(0);
					dest_items[n].data[0]			= dest_frame->data();
					dest_items[n].pix_desc			= core::pixel_format_desc(core::pixel_format::bgra);
					dest_items[n].pix_desc.planes	= boost::assign::list_of(core::pixel_format_desc::plane(width, height, 4));
					dest_items[n].transform			= source_items[n].transform;
				}
			}
		});	

		source_items = std::move(dest_items);
	}
};
		
struct image_mixer::impl : boost::noncopyable
{	
	image_renderer						renderer_;
	std::vector<core::image_transform>	transform_stack_;
	std::vector<item>					items_; // layer/stream/items
public:
	impl() 
		: transform_stack_(1)	
	{
		CASPAR_LOG(info) << L"Initialized Streaming SIMD Extensions Accelerated CPU Image Mixer";
	}

	void begin_layer(core::blend_mode blend_mode)
	{
	}
		
	void push(const core::frame_transform& transform)
	{
		transform_stack_.push_back(transform_stack_.back()*transform.image_transform);
	}
		
	void visit(const core::data_frame& frame)
	{			
		if(frame.pixel_format_desc().format == core::pixel_format::invalid)
			return;

		if(frame.pixel_format_desc().planes.empty())
			return;
		
		if(frame.pixel_format_desc().planes.at(0).size < 16)
			return;

		if(transform_stack_.back().field_mode == core::field_mode::empty)
			return;

		item item;
		item.pix_desc	= frame.pixel_format_desc();
		item.transform	= transform_stack_.back();
		for(int n = 0; n < item.pix_desc.planes.size(); ++n)
			item.data.at(n) = frame.image_data(n).begin();		

		items_.push_back(item);
	}

	void pop()
	{
		transform_stack_.pop_back();
	}

	void end_layer()
	{		
	}
	
	boost::shared_future<boost::iterator_range<const uint8_t*>> render(const core::video_format_desc& format_desc)
	{
		return renderer_(std::move(items_), format_desc);
	}
	
	virtual spl::unique_ptr<core::data_frame> create_frame(const void* tag, const core::pixel_format_desc& desc, double frame_rate, core::field_mode field_mode)
	{
		return spl::make_unique<cpu::data_frame>(tag, desc, frame_rate, field_mode);
	}
};

image_mixer::image_mixer() : impl_(new impl()){}
void image_mixer::push(const core::frame_transform& transform){impl_->push(transform);}
void image_mixer::visit(const core::data_frame& frame){impl_->visit(frame);}
void image_mixer::pop(){impl_->pop();}
boost::shared_future<boost::iterator_range<const uint8_t*>> image_mixer::operator()(const core::video_format_desc& format_desc){return impl_->render(format_desc);}
void image_mixer::begin_layer(core::blend_mode blend_mode){impl_->begin_layer(blend_mode);}
void image_mixer::end_layer(){impl_->end_layer();}
spl::unique_ptr<core::data_frame> image_mixer::create_frame(const void* tag, const core::pixel_format_desc& desc, double frame_rate, core::field_mode field_mode) {return impl_->create_frame(tag, desc, frame_rate, field_mode);}

}}}