/*
 * copyright (c) 2020 Hailong Geng <longlongh4@gmail.com>
 *
 * This file is part of Bento5.
 *
 *
 * Bento5 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bento5 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bento5.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <cxxopts.hpp>
#include <filesystem>
#include <cmath>
#include "Ap4.h"
#include "Ap4Mp4AudioInfo.h"

const uint PMT_PID = 0x100;
const uint AUDIO_PID = 0x101;
const uint VIDEO_PID = 0x102;

const char* SEGMENT_FILENAME_TEMPLATE = "segment-%d.ts";
const char* INDEX_FILENAME = "stream.m3u8";

const float MAX_DTS_DELTA = 0.2;

class Stats {
public:
    Stats(): segments_total_size(0), segments_total_duration(0.0), segment_count(0), max_segment_bitrate(0.0), codecs(""), resolution("")  {}
    AP4_UI64 segments_total_size;
    double   segments_total_duration;
    AP4_UI32 segment_count;
    double   max_segment_bitrate;
    std::string codecs;
    std::string resolution;
};

/*----------------------------------------------------------------------
|   OpenOutput
+---------------------------------------------------------------------*/
static AP4_ByteStream*
OpenOutput(std::filesystem::path out_folder, const char* filename_pattern, unsigned int segment_number)
{
    AP4_ByteStream* output = NULL;
    char filename[4096];
    sprintf(filename, filename_pattern, segment_number);
    AP4_Result result = AP4_FileByteStream::Create(std::filesystem::absolute(out_folder.append(filename)).string().c_str(), AP4_FileByteStream::STREAM_MODE_WRITE, output);
    if (AP4_FAILED(result)) {
        fprintf(stderr, "ERROR: cannot open output (%d)\n", result);
        return NULL;
    }

    return output;
}

/*----------------------------------------------------------------------
|   SampleReader
+---------------------------------------------------------------------*/
class SampleReader
{
public:
    virtual ~SampleReader() {}
    virtual AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data) = 0;
};

/*----------------------------------------------------------------------
|   TrackSampleReader
+---------------------------------------------------------------------*/
class TrackSampleReader : public SampleReader
{
public:
    TrackSampleReader(AP4_Track& track) : m_Track(track), m_SampleIndex(0) {}
    AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data);

private:
    AP4_Track&  m_Track;
    AP4_Ordinal m_SampleIndex;
};

/*----------------------------------------------------------------------
|   TrackSampleReader
+---------------------------------------------------------------------*/
AP4_Result
TrackSampleReader::ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data)
{
    if (m_SampleIndex >= m_Track.GetSampleCount()) return AP4_ERROR_EOS;
    return m_Track.ReadSample(m_SampleIndex++, sample, sample_data);
}

/*----------------------------------------------------------------------
|   FragmentedSampleReader
+---------------------------------------------------------------------*/
class FragmentedSampleReader : public SampleReader
{
public:
    FragmentedSampleReader(AP4_LinearReader& fragment_reader, AP4_UI32 track_id) :
        m_FragmentReader(fragment_reader), m_TrackId(track_id) {
        fragment_reader.EnableTrack(track_id);
    }
    AP4_Result ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data);

private:
    AP4_LinearReader& m_FragmentReader;
    AP4_UI32          m_TrackId;
};

/*----------------------------------------------------------------------
|   FragmentedSampleReader
+---------------------------------------------------------------------*/
AP4_Result
FragmentedSampleReader::ReadSample(AP4_Sample& sample, AP4_DataBuffer& sample_data)
{
    return m_FragmentReader.ReadNextSample(m_TrackId, sample, sample_data);
}

/*----------------------------------------------------------------------
|   ReadSample
+---------------------------------------------------------------------*/
static AP4_Result
ReadSample(SampleReader&   reader,
           AP4_Track&      track,
           AP4_Sample&     sample,
           AP4_DataBuffer& sample_data,
           double&         ts,
           double&         duration,
           bool&           eos)
{
    AP4_Result result = reader.ReadSample(sample, sample_data);
    if (AP4_FAILED(result)) {
        if (result == AP4_ERROR_EOS) {
            ts += duration;
            eos = true;
        } else {
            return result;
        }
    }
    ts = (double)sample.GetDts()/(double)track.GetMediaTimeScale();
    duration = sample.GetDuration()/(double)track.GetMediaTimeScale();

    return AP4_SUCCESS;
}


class InputStream {
public:
    InputStream(std::string file_path) : file_path(file_path), input(NULL), input_file(NULL), movie(NULL), audio_track(NULL), video_track(NULL),
        linear_reader(NULL), audio_reader(NULL), video_reader(NULL) {
        AP4_Result result;
        result = AP4_FileByteStream::Create(file_path.data(), AP4_FileByteStream::STREAM_MODE_READ, input);
        if (AP4_FAILED(result)) {
            fprintf(stderr, "ERROR: cannot open input (%s)\n", file_path.data());
            exit(-1);
        }
        // open the file
        input_file = new AP4_File(*input, true);

        // get the movie
        movie = input_file->GetMovie();
        if (movie == NULL) {
            fprintf(stderr, "ERROR: no movie in file %s\n", file_path.data());
            exit(-1);
        }
        // get the audio and video tracks
        audio_track = movie->GetTrack(AP4_Track::TYPE_AUDIO);
        video_track = movie->GetTrack(AP4_Track::TYPE_VIDEO);

        if (audio_track == NULL && video_track == NULL) {
            fprintf(stderr, "ERROR: no video and audio track in %s\n", file_path.data());
            exit(-1);
        }

        if (movie->HasFragments()) {
            // create a linear reader to get the samples
            linear_reader = new AP4_LinearReader(*movie, input);

            if (audio_track) {
                linear_reader->EnableTrack(audio_track->GetId());
                audio_reader = new FragmentedSampleReader(*linear_reader, audio_track->GetId());
            }
            if (video_track) {
                linear_reader->EnableTrack(video_track->GetId());
                video_reader = new FragmentedSampleReader(*linear_reader, video_track->GetId());
            }
        } else {
            if (audio_track) {
                audio_reader = new TrackSampleReader(*audio_track);
            }
            if (video_track) {
                video_reader = new TrackSampleReader(*video_track);
            }
        }
    };
    ~InputStream(){
        if(input != NULL) {
            input->Release();
        }
        delete input_file;
        delete video_reader;
        delete audio_reader;
        delete linear_reader;
    };

    std::vector<float> getKeyframesDTSTimeList() {
        std::vector<float> array;
        AP4_Track* video_track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
        if (video_track) {
            AP4_Sample sample;
            for(unsigned int i = 0; i < video_track->GetSampleCount(); i++) {
                AP4_Result result = video_track->GetSample(i, sample);
                if (AP4_FAILED(result)) {
                    fprintf(stderr, "failed to get video sample in %s", file_path.c_str());
                    exit(-1);
                }
                if (sample.IsSync()) {
                    array.push_back(float(sample.GetDts()) / video_track->GetMediaTimeScale());
                }
            }
        }
        return array;
    }
private:
    std::string file_path;
    AP4_ByteStream* input;
    AP4_File* input_file;
    AP4_Movie* movie;
    AP4_Track* audio_track;
    AP4_Track* video_track;
    AP4_LinearReader* linear_reader;
    SampleReader*     audio_reader;
    SampleReader*     video_reader;
    friend class OutputStream;
};

class OutputStream {
public:
    OutputStream(std::filesystem::path out_folder, const InputStream* input): ts_writer(NULL), audio_stream(NULL), video_stream(NULL), input_stream(input), out_folder(out_folder) {
        if (bool flag = std::filesystem::create_directories(out_folder); flag == false) {
            fprintf(stderr, "failed to create output folder at %s, maybe it already exists?\n", std::filesystem::absolute(out_folder).string().c_str());
            exit(-1);
        }
        // create an MPEG2 TS Writer
        ts_writer = new AP4_Mpeg2TsWriter(PMT_PID);
        // add the audio stream
        if (input->audio_track) {
            AP4_SampleDescription *sample_description = input->audio_track->GetSampleDescription(0);
            if (sample_description == NULL) {
                fprintf(stderr, "ERROR: unable to parse audio sample description of %s\n", input->file_path.data());
                exit(-1);
            }

            unsigned int stream_type = 0;
            unsigned int stream_id   = 0;
            if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_MP4A) {
                stream_type = AP4_MPEG2_STREAM_TYPE_ISO_IEC_13818_7;
                stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_AUDIO;
            } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AC_3) {
                stream_type = AP4_MPEG2_STREAM_TYPE_ATSC_AC3;
                stream_id   = AP4_MPEG2_TS_STREAM_ID_PRIVATE_STREAM_1;
            } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_EC_3) {
                stream_type = AP4_MPEG2_STREAM_TYPE_ATSC_EAC3;
                stream_id   = AP4_MPEG2_TS_STREAM_ID_PRIVATE_STREAM_1;
            } else {
                fprintf(stderr, "ERROR: audio codec not supported for %s\n", input->file_path.data());
                exit(-1);
            }

            // setup the audio stream
            AP4_Result result = ts_writer->SetAudioStream(input->audio_track->GetMediaTimeScale(),
                                                          stream_type,
                                                          stream_id,
                                                          audio_stream,
                                                          AUDIO_PID,
                                                          NULL, 0,
                                                          AP4_MPEG2_TS_DEFAULT_PCR_OFFSET);
            if (AP4_FAILED(result)) {
                fprintf(stderr, "could not create audio stream of %s\n", input->file_path.data());
                exit(-1);
            }
        }

        // add the video stream
        if (input->video_track) {
            AP4_SampleDescription *sample_description = input->video_track->GetSampleDescription(0);
            if (sample_description == NULL) {
                fprintf(stderr, "ERROR: unable to parse video sample description of %s\n", input->file_path.data());
                exit(-1);
            }

            // decide on the stream type
            unsigned int stream_type = 0;
            unsigned int stream_id   = AP4_MPEG2_TS_DEFAULT_STREAM_ID_VIDEO;
            if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC1 ||
                    sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC2 ||
                    sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC3 ||
                    sample_description->GetFormat() == AP4_SAMPLE_FORMAT_AVC4 ||
                    sample_description->GetFormat() == AP4_SAMPLE_FORMAT_DVAV ||
                    sample_description->GetFormat() == AP4_SAMPLE_FORMAT_DVA1) {
                stream_type = AP4_MPEG2_STREAM_TYPE_AVC;
            } else if (sample_description->GetFormat() == AP4_SAMPLE_FORMAT_HEV1 ||
                       sample_description->GetFormat() == AP4_SAMPLE_FORMAT_HVC1 ||
                       sample_description->GetFormat() == AP4_SAMPLE_FORMAT_DVHE ||
                       sample_description->GetFormat() == AP4_SAMPLE_FORMAT_DVH1) {
                stream_type = AP4_MPEG2_STREAM_TYPE_HEVC;
            } else {
                fprintf(stderr, "ERROR: video codec not supported for %s\n", input->file_path.data());
                exit(-1);
            }

            // setup the video stream
            AP4_Result result = ts_writer->SetVideoStream(input->video_track->GetMediaTimeScale(),
                                                          stream_type,
                                                          stream_id,
                                                          video_stream,
                                                          VIDEO_PID,
                                                          NULL, 0,
                                                          AP4_MPEG2_TS_DEFAULT_PCR_OFFSET);
            if (AP4_FAILED(result)) {
                fprintf(stderr, "could not create video stream of %s\n", input->file_path.data());
                exit(-1);
            }
        }
    };
    ~OutputStream() {
        delete ts_writer;
        delete input_stream;
    };

    static AP4_Result write_samples(OutputStream *output, float seg_duration, std::vector<float> segmentPoints) {
        AP4_Sample              audio_sample;
        AP4_DataBuffer          audio_sample_data;
        unsigned int            audio_sample_count = 0;
        double                  audio_ts = 0.0;
        double                  audio_frame_duration = 0.0;
        bool                    audio_eos = false;
        AP4_Sample              video_sample;
        AP4_DataBuffer          video_sample_data;
        unsigned int            video_sample_count = 0;
        double                  video_ts = 0.0;
        double                  video_frame_duration = 0.0;
        bool                    video_eos = false;
        double                  last_ts = 0.0;
        unsigned int            segment_number = 0;
        AP4_ByteStream*         segment_output = NULL;
        double                  segment_duration = 0.0;
        AP4_Array<double>       segment_durations;
        AP4_Array<AP4_UI32>     segment_sizes;
        AP4_Position            segment_position = 0;
        AP4_Array<AP4_Position> segment_positions;
        bool                    new_segment = true;
        AP4_ByteStream*         playlist = NULL;
        char                    string_buffer[4096];
        AP4_Result              result = AP4_SUCCESS;

        const InputStream *input = output->input_stream;

        // prime the samples
        if (input->audio_reader) {
            result = ReadSample(*input->audio_reader, *input->audio_track, audio_sample, audio_sample_data, audio_ts, audio_frame_duration, audio_eos);
            if (AP4_FAILED(result)) return result;
        }
        if (input->video_reader) {
            result = ReadSample(*input->video_reader, *input->video_track, video_sample, video_sample_data, video_ts, video_frame_duration, video_eos);
            if (AP4_FAILED(result)) return result;
        }

        for (;;) {
            bool sync_sample = false;
            AP4_Track* chosen_track= NULL;
            if (input->audio_track && !audio_eos) {
                chosen_track = input->audio_track;
                if (input->video_track == NULL) sync_sample = true;
            }
            if (input->video_track && !video_eos) {
                if (input->audio_track) {
                    if (video_ts <= audio_ts) {
                        chosen_track = input->video_track;
                    }
                } else {
                    chosen_track = input->video_track;
                }
                if (chosen_track == input->video_track && video_sample.IsSync()) {
                    sync_sample = true;
                }
            }

            // check if we need to start a new segment
            if (seg_duration && (sync_sample || chosen_track == NULL)) {
                if (input->video_track) {
                    segment_duration = video_ts - last_ts;
                } else {
                    segment_duration = audio_ts - last_ts;
                }
                if ( (input->video_track == NULL && segment_duration >= seg_duration)
                     || (input->video_track != NULL && std::find_if(segmentPoints.begin(), segmentPoints.end(), [video_ts](float x) {return abs(x - video_ts) <= 2 * MAX_DTS_DELTA; }) != segmentPoints.end())
                     || chosen_track == NULL) {
                    if (input->video_track) {
                        last_ts = video_ts;
                    } else {
                        last_ts = audio_ts;
                    }
                    if (segment_output) {
                        // flush the output stream
                        segment_output->Flush();

                        // compute the segment size (including padding)
                        AP4_Position segment_end = 0;
                        segment_output->Tell(segment_end);
                        AP4_UI32 segment_size = 0;
                        if (segment_end > segment_position) {
                            segment_size = (AP4_UI32)(segment_end-segment_position);
                        }

                        // update counters
                        segment_sizes.Append(segment_size);
                        segment_positions.Append(segment_position);
                        segment_durations.Append(segment_duration);

                        if (abs(segment_duration) > 0.0) {
                            double segment_bitrate = 8.0*(double)segment_size/segment_duration;
                            if (segment_bitrate > output->stats.max_segment_bitrate) {
                                output->stats.max_segment_bitrate = segment_bitrate;
                            }
                        }
                        segment_output->Release();
                        segment_output = NULL;

                        ++segment_number;
                        audio_sample_count = 0;
                        video_sample_count = 0;
                    }
                    new_segment = true;
                }
            }

            // check if we're done
            if (chosen_track == NULL) break;

            if (new_segment) {
                new_segment = false;

                // compute the new segment position
                segment_position = 0;

                // manage the new segment stream
                if (segment_output == NULL) {
                    segment_output = OpenOutput(output->out_folder, SEGMENT_FILENAME_TEMPLATE, segment_number);
                    if (segment_output == NULL) return AP4_ERROR_CANNOT_OPEN_FILE;
                }

                // write the PAT and PMT
                if (output->ts_writer) {
                    output->ts_writer->WritePAT(*segment_output);
                    output->ts_writer->WritePMT(*segment_output);
                }
            }

            // write the samples out and advance to the next sample
            if (chosen_track == input->audio_track) {

                // write the sample data
                if (output->audio_stream) {
                    result = output->audio_stream->WriteSample(audio_sample,
                                                               audio_sample_data,
                                                               input->audio_track->GetSampleDescription(audio_sample.GetDescriptionIndex()),
                                                               input->video_track==NULL,
                                                               *segment_output);
                } else {
                    return AP4_ERROR_INTERNAL;
                }
                if (AP4_FAILED(result)) return result;

                result = ReadSample(*input->audio_reader, *input->audio_track, audio_sample, audio_sample_data, audio_ts, audio_frame_duration, audio_eos);
                if (AP4_FAILED(result)) return result;
                ++audio_sample_count;
            } else if (chosen_track == input->video_track) {
                // write the sample data
                AP4_Position frame_start = 0;
                segment_output->Tell(frame_start);
                result = output->video_stream->WriteSample(video_sample,
                                                           video_sample_data,
                                                           input->video_track->GetSampleDescription(video_sample.GetDescriptionIndex()),
                                                           true,
                                                           *segment_output);
                if (AP4_FAILED(result)) return result;
                AP4_Position frame_end = 0;
                segment_output->Tell(frame_end);

                // read the next sample
                result = ReadSample(*input->video_reader, *input->video_track, video_sample, video_sample_data, video_ts, video_frame_duration, video_eos);
                if (AP4_FAILED(result)) return result;
                ++video_sample_count;
            } else {
                break;
            }
        }

        // create the media playlist/index file
        playlist = OpenOutput(output->out_folder, INDEX_FILENAME, 0);
        if (playlist == NULL) return AP4_ERROR_CANNOT_OPEN_FILE;

        unsigned int target_duration = 0;
        double       total_duration = 0.0;
        for (unsigned int i=0; i<segment_durations.ItemCount(); i++) {
            if ((unsigned int)(segment_durations[i]+0.5) > target_duration) {
                target_duration = (unsigned int)segment_durations[i];
            }
            total_duration += segment_durations[i];
        }

        playlist->WriteString("#EXTM3U\r\n");
        sprintf(string_buffer, "#EXT-X-VERSION:%d\r\n", 3);
        playlist->WriteString(string_buffer);
        playlist->WriteString("#EXT-X-PLAYLIST-TYPE:VOD\r\n");
        if (input->video_track) {
            playlist->WriteString("#EXT-X-INDEPENDENT-SEGMENTS\r\n");
        }
        playlist->WriteString("#EXT-X-TARGETDURATION:");
        sprintf(string_buffer, "%d\r\n", target_duration);
        playlist->WriteString(string_buffer);
        playlist->WriteString("#EXT-X-MEDIA-SEQUENCE:0\r\n");

        for (unsigned int i=0; i<segment_durations.ItemCount(); i++) {
            sprintf(string_buffer, "#EXTINF:%f,\r\n", segment_durations[i]);
            playlist->WriteString(string_buffer);
            sprintf(string_buffer, SEGMENT_FILENAME_TEMPLATE, i);
            playlist->WriteString(string_buffer);
            playlist->WriteString("\r\n");
        }

        playlist->WriteString("#EXT-X-ENDLIST\r\n");
        playlist->Release();

        // update stats
        output->stats.segment_count = segment_sizes.ItemCount();
        output->stats.segments_total_duration = total_duration;
        for (unsigned int i=0; i<segment_sizes.ItemCount(); i++) {
            output->stats.segments_total_size  += segment_sizes[i];
        }

        // get codecs and resolution
        std::vector<std::string> codecs;
        if(input->audio_track) {
            AP4_String codec;
            AP4_SampleDescription *sdesc = input->audio_track->GetSampleDescription(0);
            if (sdesc) {
                sdesc ->GetCodecString(codec);
                codecs.push_back(std::string(codec.GetChars()));
            }
        }
        if(input->video_track) {
            AP4_String codec;
            AP4_SampleDescription *sdesc = input->video_track->GetSampleDescription(0);
            if (sdesc) {
                sdesc ->GetCodecString(codec);
                codecs.push_back(std::string(codec.GetChars()));
            }
            char buffer[1024];
            AP4_VideoSampleDescription* vsd = AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, input->video_track->GetSampleDescription(0));
            sprintf(buffer, "%dx%d", vsd->GetWidth(), vsd->GetHeight());
            output->stats.resolution = std::string(buffer);
        }
        std::ostringstream ss_codecs;
        std::copy(codecs.rbegin(),codecs.rend(), std::ostream_iterator<std::string>(ss_codecs,","));
        output->stats.codecs = ss_codecs.str().substr(0, ss_codecs.str().size()-1);

        if (segment_output) segment_output->Release();
        return result;
    }
    static AP4_Result generateMasterPlaylist(std::vector<OutputStream*> output_streams, std::filesystem::path output_dir) {
        AP4_ByteStream* playlist = OpenOutput(output_dir, "master.m3u8", 0);
        if (playlist == NULL) return AP4_ERROR_CANNOT_OPEN_FILE;


        playlist->WriteString("#EXTM3U\r\n");
        playlist->WriteString("# Created with Bento5 mov2hls\r\n\r\n");
        playlist->WriteString("# Media Playlists\r\n");

        std::for_each(output_streams.begin(), output_streams.end(), [playlist](OutputStream* os) {
            char string_buffer[4096];
            sprintf(string_buffer, "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=%d,BANDWIDTH=%d,CODECS=\"%s\"", int(ceil(8.0 * os->stats.segments_total_size/os->stats.segments_total_duration)), int(ceil(os->stats.max_segment_bitrate)), os->stats.codecs.c_str());
            playlist->WriteString(string_buffer);
            if (os->input_stream->video_track) {
                sprintf(string_buffer, ",RESOLUTION=%s", os->stats.resolution.c_str());
                playlist->WriteString(string_buffer);
            }
            playlist->WriteString("\r\n");
            sprintf(string_buffer, "%s/stream.m3u8\r\n", os->out_folder.filename().string().c_str());
            playlist->WriteString(string_buffer);
        });
        return AP4_SUCCESS;
    }


private:
    AP4_Mpeg2TsWriter*               ts_writer;
    AP4_Mpeg2TsWriter::SampleStream* audio_stream;
    AP4_Mpeg2TsWriter::SampleStream* video_stream;
    const InputStream *input_stream;
    std::filesystem::path out_folder;
    Stats stats;
};

class VectorCommonFloatFinder {
public:
    VectorCommonFloatFinder(std::vector<float> vec) : vec(vec), index(0) {}
    bool exist(float value) {
        while(index < vec.size()) {
            if (abs(vec.at(index) - value) < MAX_DTS_DELTA) {
                return true;
            }
            if(vec.at(index) > value) {
                break;
            }
            index++;
        }
        return false;
    }
private:
    std::vector<float> vec;
    unsigned int index;
};

std::vector<float> findAlignedDTS(std::vector<std::vector<float>> array) {
    if (array.size() == 0) {
        return std::vector<float>();
    } else if (array.size() == 1) {
        return array.at(0);
    } else {
        std::vector<float> res;
        std::vector<float> front = array.front();
        std::vector<VectorCommonFloatFinder*> finderArray;
        std::transform(array.begin(), array.end(), std::back_inserter(finderArray), [](std::vector<float> x) {return new VectorCommonFloatFinder(x);});
        for (unsigned int i = 0; i < front.size(); i++) {
            bool not_exist = false;
            for(unsigned int j = 0; j < finderArray.size(); j++) {
                if(finderArray.at(j)->exist(front.at(i))==false) {
                    not_exist = true;
                    break;
                }
            }
            if(not_exist == false) {
                res.push_back(front.at(i));
            }
        }
        std::for_each(finderArray.begin(), finderArray.end(), [](VectorCommonFloatFinder *ptr) {delete ptr;});
        return res;
    }
}

std::vector<float> filterDTSBySegmentDuration(std::vector<float> array, float segment_duration) {
    float lastDTS = 0;
    std::vector<float> res;
    for (unsigned int i = 0; i < array.size(); i++) {
        if ((array.at(i)-lastDTS) >= segment_duration || abs(array.at(i)-lastDTS-segment_duration) < 1) {
            res.push_back(array.at(i));
            lastDTS = array.at(i);
        }
    }
    return res;
}

int main(int argc, char** argv)
{
    cxxopts::Options options("mov2hls", "MOV/MP4 to HLS v3 stream");

    options.add_options()
            ("i,input-files", "Input files, separated by , eg: 1.mp4,2.mp4,3.mp4", cxxopts::value<std::vector<std::string>>())
            ("o,output-dir", "Output directory", cxxopts::value<std::string>())
            ("segment-duration", "Segment duration", cxxopts::value<double>()->default_value("6"))
            ("master-playlist", "Master Playlist name", cxxopts::value<std::string>()->default_value("master.m3u8"))
            ("v,verbose", "Be verbose (default: false)", cxxopts::value<bool>()->default_value("false"))
            ("h,help", "Print usage")
            ;

    auto result = options.parse(argc, argv);

    if (result.count("help") || result.count("input-files") == 0 || result.count("output-dir") == 0)
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    std::vector<std::string> file_paths = result["input-files"].as<std::vector<std::string>>();
    std::vector<InputStream*> input_streams;
    std::transform(file_paths.begin(), file_paths.end(), std::back_inserter(input_streams), [](std::string s) {return new InputStream(s);});

    std::vector<OutputStream*> output_streams;
    for (unsigned int i = 0; i < input_streams.size(); i++) {
        std::ostringstream out_folder;
        out_folder << "output/media-" << i;
        std::filesystem::path file_path(result["output-dir"].as<std::string>());
        output_streams.push_back(new OutputStream(file_path.append(out_folder.str()), input_streams.at(i)));
    }

    std::vector<std::vector<float>> keyframeDTS;
    std::transform(input_streams.begin(), input_streams.end(), std::back_inserter(keyframeDTS), [](InputStream *input) {return input->getKeyframesDTSTimeList();});

    std::vector<float> alignedDTS = findAlignedDTS(keyframeDTS);
    std::vector<float> filterdDTSByDuration = filterDTSBySegmentDuration(alignedDTS, result["segment-duration"].as<double>());
    std::for_each(output_streams.begin(), output_streams.end(), [result, filterdDTSByDuration](OutputStream* output_stream) {
        OutputStream::write_samples(output_stream, result["segment-duration"].as<double>(), filterdDTSByDuration);
    });

    std::filesystem::path output_folder(result["output-dir"].as<std::string>());
    AP4_Result res = OutputStream::generateMasterPlaylist(output_streams, output_folder.append("output"));
    if (AP4_FAILED(res)) {
        fprintf(stderr, "could not master playlist\n");
        exit(-1);
    }

    // clean up
    std::for_each(output_streams.begin(), output_streams.end(), [](OutputStream *ptr) {delete ptr;});
    return 0;
}
