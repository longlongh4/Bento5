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
#include "Ap4.h"

const uint PMT_PID = 0x100;
const uint AUDIO_PID = 0x101;
const uint VIDEO_PID = 0x102;

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
    OutputStream(std::string folder, const InputStream* input): ts_writer(NULL), audio_stream(NULL), video_stream(NULL), input_stream(input) {
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
    };
private:
    AP4_Mpeg2TsWriter*               ts_writer;
    AP4_Mpeg2TsWriter::SampleStream* audio_stream;
    AP4_Mpeg2TsWriter::SampleStream* video_stream;
    const InputStream *input_stream;
};

int main(int argc, char** argv)
{
    cxxopts::Options options("mov2hls", "MOV/MP4 to HLS v3 stream");

    options.add_options()
            ("i,input-files", "Input files, separated by , eg: 1.mp4,2.mp4,3.mp4", cxxopts::value<std::vector<std::string>>())
            ("o,output-dir", "Output directory", cxxopts::value<std::string>())
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

    // clean up
    std::for_each(input_streams.begin(), input_streams.end(), [](InputStream *ptr) {delete ptr;});
    return 0;
}
