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

int main(int argc, char** argv)
{
    cxxopts::Options options("mov2hls", "MOV/MP4 to HLS stream");

    options.add_options()
            ("i,input-files", "Input files, separated by , eg: 1.mp4,2.mp4,3.mp4", cxxopts::value<std::vector<std::string>>())
            ("o,output-dir", "Output directory", cxxopts::value<std::string>())
            ("hls-version", "HLS Version", cxxopts::value<std::string>()->default_value("3"))
            ("master-playlist-name", "Master Playlist name", cxxopts::value<std::string>()->default_value("master.m3u8"))
            ("output-single-file", "Store segment data in a sigle output file (default: false)", cxxopts::value<bool>()->default_value("false"))
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

    return 0;
}
