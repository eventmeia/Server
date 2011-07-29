#include "../StdAfx.h"

#include "frame_muxer.h"

#include "frame/basic_frame.h"
#include "frame/image_transform.h"
#include "frame/pixel_format.h"
#include "../mixer/write_frame.h"

#include <common/env.h>
#include <common/log/log.h>

#include <boost/range/algorithm_ext/push_back.hpp>

namespace caspar { namespace core {
	
struct display_mode
{
	enum type
	{
		simple,
		duplicate,
		half,
		interlace,
		deinterlace,
		deinterlace_half,
		count,
		invalid
	};

	static std::wstring print(display_mode::type value)
	{
		switch(value)
		{
			case simple:
				return L"simple";
			case duplicate:
				return L"duplicate";
			case half:
				return L"half";
			case interlace:
				return L"interlace";
			case deinterlace:
				return L"deinterlace";
			case deinterlace_half:
				return L"deinterlace_half";
			default:
				return L"invalid";
		}
	}
};

display_mode::type get_display_mode(const core::video_mode::type in_mode, double in_fps, const core::video_mode::type out_mode, double out_fps)
{		
	if(in_mode == core::video_mode::invalid || out_mode == core::video_mode::invalid)
		return display_mode::invalid;

	static const auto epsilon = 2.0;

	if(std::abs(in_fps - out_fps) < epsilon)
	{
		if(in_mode != core::video_mode::progressive && out_mode == core::video_mode::progressive)
			return display_mode::deinterlace_half;
		//else if(in_mode == core::video_mode::progressive && out_mode != core::video_mode::progressive)
		//	simple(); // interlace_duplicate();
		else
			return display_mode::simple;
	}
	else if(std::abs(in_fps/2.0 - out_fps) < epsilon)
	{
		if(in_mode != core::video_mode::progressive)
			return display_mode::invalid;

		if(out_mode != core::video_mode::progressive)
			return display_mode::interlace;
		else
			return display_mode::half;
	}
	else if(std::abs(in_fps - out_fps/2.0) < epsilon)
	{
		if(out_mode != core::video_mode::progressive)
			return display_mode::invalid;

		if(in_mode != core::video_mode::progressive)
			return display_mode::deinterlace;
		else
			return display_mode::duplicate;
	}

	return display_mode::invalid;
}

struct frame_muxer::implementation
{	
	std::queue<safe_ptr<write_frame>>	video_frames_;
	std::vector<int16_t>				audio_samples_;
	std::queue<safe_ptr<basic_frame>>	frame_buffer_;
	display_mode::type					display_mode_;
	const double						in_fps_;
	const video_format_desc				format_desc_;
	bool								auto_mode_;

	size_t								audio_sample_count_;

	size_t								video_frame_count_;
		
	implementation(double in_fps, const video_format_desc& format_desc)
		: display_mode_(display_mode::invalid)
		, in_fps_(in_fps)
		, format_desc_(format_desc)
		, auto_mode_(env::properties().get("configuration.auto-mode", false))
		, audio_sample_count_(0)
		, video_frame_count_(0)
	{
	}

	void push(const std::shared_ptr<write_frame>& video_frame)
	{		
		if(!video_frame)
		{	
			CASPAR_LOG(debug) << L"video-frame-count: " << video_frame_count_;
			video_frame_count_ = 0;
			return;
		}

		++video_frame_count_;

		// Fix field-order if needed
		if(video_frame->get_type() == core::video_mode::lower && format_desc_.mode == core::video_mode::upper)
			video_frame->get_image_transform().set_fill_translation(0.0f, 0.5/static_cast<double>(video_frame->get_pixel_format_desc().planes[0].height));
		else if(video_frame->get_type() == core::video_mode::upper && format_desc_.mode == core::video_mode::lower)
			video_frame->get_image_transform().set_fill_translation(0.0f, -0.5/static_cast<double>(video_frame->get_pixel_format_desc().planes[0].height));

		video_frames_.push(make_safe(video_frame));

		process();
	}

	void push(const std::shared_ptr<std::vector<int16_t>>& audio_samples)
	{
		if(!audio_samples)	
		{
			auto truncate = audio_sample_count_ % format_desc_.audio_samples_per_frame;
			if(truncate > 0)
			{
				audio_samples_.erase(audio_samples_.end() - truncate, audio_samples_.end());
				CASPAR_LOG(info) << L"frame_muxer: Truncating " << truncate << L" audio samples.";
			}

			CASPAR_LOG(debug) << L"audio-chunk-count: " << audio_sample_count_/format_desc_.audio_samples_per_frame;
			audio_sample_count_ = 0;
			return;
		}

		audio_sample_count_ += audio_samples->size();

		boost::range::push_back(audio_samples_, *audio_samples);
		process();
	}

	safe_ptr<basic_frame> pop()
	{		
		auto frame = frame_buffer_.front();
		frame_buffer_.pop();
		return frame;
	}

	size_t size() const
	{
		return frame_buffer_.size();
	}

	safe_ptr<core::write_frame> pop_video()
	{
		auto frame = video_frames_.front();
		video_frames_.pop();
		return frame;
	}

	std::vector<int16_t> pop_audio()
	{
		CASPAR_VERIFY(audio_samples_.size() >= format_desc_.audio_samples_per_frame);

		auto begin = audio_samples_.begin();
		auto end   = begin + format_desc_.audio_samples_per_frame;

		auto samples = std::vector<int16_t>(begin, end);
		audio_samples_.erase(begin, end);

		return samples;
	}

	void process()
	{
		if(video_frames_.empty() || audio_samples_.size() < format_desc_.audio_samples_per_frame)
			return;

		if(display_mode_ == display_mode::invalid)
			display_mode_ = auto_mode_ ? get_display_mode(video_frames_.front()->get_type(), in_fps_, format_desc_.mode, format_desc_.fps) : display_mode::simple;

		switch(display_mode_)
		{
		case display_mode::simple:
			return simple();
		case display_mode::duplicate:
			return duplicate();
		case display_mode::half:
			return half();
		case display_mode::interlace:
			return interlace();
		case display_mode::deinterlace:
			return deinterlace();
		case display_mode::deinterlace_half:
			return deinterlace_half();
		default:
			BOOST_THROW_EXCEPTION(invalid_operation());
		}
	}

	void simple()
	{
		if(video_frames_.empty() || audio_samples_.size() < format_desc_.audio_samples_per_frame)
			return;

		auto frame1 = pop_video();
		frame1->audio_data() = pop_audio();

		frame_buffer_.push(frame1);
	}

	void duplicate()
	{		
		if(video_frames_.empty() || audio_samples_.size()/2 < format_desc_.audio_samples_per_frame)
			return;

		auto frame = pop_video();

		auto frame1 = make_safe<core::write_frame>(*frame); // make a copy
		frame1->audio_data() = pop_audio();

		auto frame2 = frame;
		frame2->audio_data() = pop_audio();

		frame_buffer_.push(frame1);
		frame_buffer_.push(frame2);
	}

	void half()
	{	
		if(video_frames_.size() < 2 || audio_samples_.size() < format_desc_.audio_samples_per_frame)
			return;
						
		auto frame1 = pop_video();
		frame1->audio_data() = pop_audio();
				
		video_frames_.pop(); // Throw away

		frame_buffer_.push(frame1);
	}
	
	void interlace()
	{		
		if(video_frames_.size() < 2 || audio_samples_.size() < format_desc_.audio_samples_per_frame)
			return;
		
		auto frame1 = pop_video();

		frame1->audio_data() = pop_audio();
				
		auto frame2 = pop_video();

		frame_buffer_.push(core::basic_frame::interlace(frame1, frame2, format_desc_.mode));		
	}
	
	void deinterlace()
	{
		BOOST_THROW_EXCEPTION(not_implemented() << msg_info("deinterlace"));
	}

	void deinterlace_half()
	{
		BOOST_THROW_EXCEPTION(not_implemented() << msg_info("deinterlace_half"));
	}
};

frame_muxer::frame_muxer(double in_fps, const video_format_desc& format_desc)
	: impl_(implementation(in_fps, format_desc)){}
void frame_muxer::push(const std::shared_ptr<write_frame>& video_frame){impl_->push(video_frame);}
void frame_muxer::push(const std::shared_ptr<std::vector<int16_t>>& audio_samples){return impl_->push(audio_samples);}
safe_ptr<basic_frame> frame_muxer::pop(){return impl_->pop();}
size_t frame_muxer::size() const {return impl_->size();}
bool frame_muxer::empty() const {return impl_->size() == 0;}
size_t frame_muxer::video_frames() const{return impl_->video_frames_.size();}
size_t frame_muxer::audio_chunks() const{return impl_->audio_samples_.size() / impl_->format_desc_.audio_samples_per_frame;}

}}