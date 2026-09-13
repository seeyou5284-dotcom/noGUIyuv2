/*
 * YUV Raw Image/Video CLI Inspector & Differencer
 * Headless C++ utility (No GUI / SDL required)
 * Version 4: Adds pixel-level (x,y) coordinate diff inspection for YUV stream comparison.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// YUV BT.601 limited-range -> RGB
// ---------------------------------------------------------------------------
struct RGB {
    uint8_t r, g, b;
};

static RGB yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v) {
    int c = (int)y - 16, d = (int)u - 128, e = (int)v - 128;
    auto clamp8 = [](int x) -> uint8_t {
        return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
    };
    return {
        clamp8((298 * c + 409 * e + 128) >> 8),
        clamp8((298 * c - 100 * d - 208 * e + 128) >> 8),
        clamp8((298 * c + 516 * d + 128) >> 8)
    };
}

// ---------------------------------------------------------------------------
// Chroma subsampling: 4:4:4, 4:2:2, 4:2:0
// ---------------------------------------------------------------------------
static void chroma_shifts(int chroma, int &shift_x, int &shift_y) {
    switch (chroma) {
    case 444:
        shift_x = 0; shift_y = 0; break;
    case 422:
        shift_x = 1; shift_y = 0; break;
    default: // 420
        shift_x = 1; shift_y = 1; break;
    }
}

static bool dims_ok(int w, int h, int chroma) {
    if (w <= 0 || h <= 0) return false;
    int sx, sy;
    chroma_shifts(chroma, sx, sy);
    if (sx && (w & 1)) return false;
    if (sy && (h & 1)) return false;
    return true;
}

static bool is_number(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit((unsigned char)c)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-frame size & depth info (.chk parser)
// ---------------------------------------------------------------------------
struct ChkFrameInfo {
    int width, height, bit_depth;
};

static std::vector<ChkFrameInfo> parse_chk_frames(const std::string &path) {
    std::vector<ChkFrameInfo> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;

    std::string line;
    bool have_size = false;
    int pend_w = 0, pend_h = 0;

    while (std::getline(f, line)) {
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;

        if (line.compare(p, 5, "Size:") == 0) {
            std::istringstream iss(line.substr(p + 5));
            if (iss >> pend_w >> pend_h) have_size = true;
            continue;
        }

        if (have_size && line.compare(p, 9, "DepthYUV:") == 0) {
            std::istringstream iss(line.substr(p + 9));
            int dy, du, dv;
            if (iss >> dy >> du >> dv) {
                int bd = (dy > 8 || du > 8 || dv > 8) ? 10 : 8;
                out.push_back({pend_w, pend_h, bd});
            }
            have_size = false;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// CLI Options Struct
// ---------------------------------------------------------------------------
struct Options {
    std::string input1;
    std::string input2;
    std::string chk_file;
    std::string export_ppm_prefix;
    int width = 384;
    int height = 256;
    int bit_depth = 8;
    int chroma = 420;
    int target_frame = -1;
    int inspect_x = -1;
    int inspect_y = -1;
    int search_y = -1;
    int search_u = -1;
    int search_v = -1;
    bool diff_mode = false;
    bool stats_mode = false;
    int max_diff_matches = 20;
};

static void print_usage(const char *prog) {
    std::cout << "\nHeadless YUV Raw Video Inspector & Tool (v4)\n"
              << "--------------------------------------------\n"
              << "Positional Usage Examples:\n"
              << "  1 Single file:         " << prog << " input.yuv 1920 1080\n"
              << "  2 Two files (--diff):  " << prog << " primary.yuv reference.yuv 1920 1080\n"
              << "  3 Three files:         " << prog << " primary.yuv reference.yuv metadata.chk 1920 1080\n\n"
              << "Flag Options:\n"
              << "  --in <file>           Primary YUV input file\n"
              << "  --in2 <file>          Secondary YUV file (for --diff mode)\n"
              << "  --chk <file.chk>      Companion metadata .chk file\n"
              << "  --width <w>           Frame width (default: 384)\n"
              << "  --height <h>          Frame height (default: 256)\n"
              << "  --depth <8|10>        Bit depth (default: 8)\n"
              << "  --chroma <420|422|444> Chroma subsampling format (default: 420)\n"
              << "  --frame <num>         Target frame index (0-based, default: all)\n"
              << "  --pixel <x> <y>       Inspect YUV and RGB values at (x,y)\n"
              << "  --find-y <val>        Search for pixels matching Y value\n"
              << "  --find-u <val>        Search for pixels matching U value\n"
              << "  --find-v <val>        Search for pixels matching V value\n"
              << "  --diff                Enable frame comparison between primary and secondary files\n"
              << "  --max-diff <num>      Max pixel diff (x,y) coordinates to print (default: 20, 0=all)\n"
              << "  --export-ppm <prefix> Export frame(s) to PPM image file(s)\n"
              << "  --stats               Output YUV plane min/max/average statistics\n"
              << "  --help, -h            Show this usage message\n\n";
}

static bool save_ppm(const std::string &filename, const std::vector<uint8_t> &rgb, int width, int height) {
    std::ofstream f(filename, std::ios::binary);
    if (!f.is_open()) return false;
    f << "P6\n" << width << " " << height << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    Options opts;
    int num_count = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--in" && i + 1 < argc) {
            opts.input1 = argv[++i];
        } else if (arg == "--in2" && i + 1 < argc) {
            opts.input2 = argv[++i];
            opts.diff_mode = true;
        } else if (arg == "--chk" && i + 1 < argc) {
            opts.chk_file = argv[++i];
        } else if (arg == "--width" && i + 1 < argc) {
            opts.width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            opts.height = std::stoi(argv[++i]);
        } else if (arg == "--depth" && i + 1 < argc) {
            opts.bit_depth = std::stoi(argv[++i]);
        } else if (arg == "--chroma" && i + 1 < argc) {
            opts.chroma = std::stoi(argv[++i]);
        } else if (arg == "--frame" && i + 1 < argc) {
            opts.target_frame = std::stoi(argv[++i]);
        } else if (arg == "--pixel" && i + 2 < argc) {
            opts.inspect_x = std::stoi(argv[++i]);
            opts.inspect_y = std::stoi(argv[++i]);
        } else if (arg == "--find-y" && i + 1 < argc) {
            opts.search_y = std::stoi(argv[++i]);
        } else if (arg == "--find-u" && i + 1 < argc) {
            opts.search_u = std::stoi(argv[++i]);
        } else if (arg == "--find-v" && i + 1 < argc) {
            opts.search_v = std::stoi(argv[++i]);
        } else if (arg == "--diff") {
            opts.diff_mode = true;
        } else if (arg == "--max-diff" && i + 1 < argc) {
            opts.max_diff_matches = std::stoi(argv[++i]);
        } else if (arg == "--stats") {
            opts.stats_mode = true;
        } else if (arg == "--export-ppm" && i + 1 < argc) {
            opts.export_ppm_prefix = argv[++i];
        } else if (arg[0] != '-') {
            // Positional argument handling
            if (!is_number(arg)) {
                // Positional File Argument
                if (arg.length() >= 4 && arg.substr(arg.length() - 4) == ".chk") {
                    opts.chk_file = arg;
                } else if (opts.input1.empty()) {
                    opts.input1 = arg;
                } else if (opts.input2.empty()) {
                    opts.input2 = arg;
                    opts.diff_mode = true;
                } else if (opts.chk_file.empty()) {
                    opts.chk_file = arg;
                }
            } else {
                // Positional Numeric Argument
                int val = std::stoi(arg);
                num_count++;
                if (num_count == 1) opts.width = val;
                else if (num_count == 2) opts.height = val;
                else if (num_count == 3 && (val == 8 || val == 10)) opts.bit_depth = val;
                else if (num_count == 4 && (val == 420 || val == 422 || val == 444)) opts.chroma = val;
            }
        }
    }

    if (opts.input1.empty()) {
        std::cerr << "Error: Primary input file is required.\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!dims_ok(opts.width, opts.height, opts.chroma)) {
        std::cerr << "Error: Invalid dimensions (" << opts.width << "x" << opts.height
                  << ") for chroma format " << opts.chroma << ".\n";
        return 1;
    }

    // Parse metadata file if present
    std::vector<ChkFrameInfo> chk_frames;
    std::vector<long long> frame_offsets;
    if (!opts.chk_file.empty()) {
        chk_frames = parse_chk_frames(opts.chk_file);
        if (chk_frames.empty()) {
            std::cerr << "Warning: Could not parse frames from .chk file: " << opts.chk_file << "\n";
        } else {
            int sx, sy;
            chroma_shifts(opts.chroma, sx, sy);
            long long cur_off = 0;
            for (const auto &fr : chk_frames) {
                frame_offsets.push_back(cur_off);
                int uv_w_i = fr.width >> sx;
                int uv_h_i = fr.height >> sy;
                int bps_i = (fr.bit_depth > 8) ? 2 : 1;
                long long fsz = (fr.width * fr.height + 2 * uv_w_i * uv_h_i) * bps_i;
                cur_off += fsz;
            }
            std::cout << "Loaded " << chk_frames.size() << " frame descriptors from " << opts.chk_file << "\n";
        }
    }

    // Open file 1
    std::ifstream f1(opts.input1, std::ios::binary | std::ios::ate);
    if (!f1.is_open()) {
        std::cerr << "Error: Cannot open input file: " << opts.input1 << "\n";
        return 1;
    }
    long long sz1 = f1.tellg();

    // Open file 2 if diff mode
    std::ifstream f2;
    long long sz2 = 0;
    if (opts.diff_mode) {
        if (opts.input2.empty()) {
            std::cerr << "Error: --diff mode requires secondary input file (--in2 or positional 2nd file).\n";
            return 1;
        }
        f2.open(opts.input2, std::ios::binary | std::ios::ate);
        if (!f2.is_open()) {
            std::cerr << "Error: Cannot open secondary input file: " << opts.input2 << "\n";
            return 1;
        }
        sz2 = f2.tellg();
    }

    // Calculate frame geometry
    int shift_x, shift_y;
    chroma_shifts(opts.chroma, shift_x, shift_y);
    int default_uv_w = opts.width >> shift_x;
    int default_uv_h = opts.height >> shift_y;
    int default_bps = (opts.bit_depth > 8) ? 2 : 1;
    long long default_frame_sz = (opts.width * opts.height + 2 * default_uv_w * default_uv_h) * default_bps;

    int total_frames1 = chk_frames.empty() ? (int)(sz1 / default_frame_sz) : (int)chk_frames.size();
    int total_frames2 = opts.diff_mode ? (chk_frames.empty() ? (int)(sz2 / default_frame_sz) : (int)chk_frames.size()) : 0;

    std::cout << "Primary Input: " << opts.input1 << " (" << total_frames1 << " frames)\n";
    if (opts.diff_mode) {
        std::cout << "Secondary Input (--diff): " << opts.input2 << " (" << total_frames2 << " frames)\n";
    }
    if (!opts.chk_file.empty()) {
        std::cout << "Metadata File: " << opts.chk_file << "\n";
    }

    int start_frame = (opts.target_frame >= 0) ? opts.target_frame : 0;
    int end_frame = (opts.target_frame >= 0) ? opts.target_frame + 1 : total_frames1;

    for (int frame_idx = start_frame; frame_idx < end_frame && frame_idx < total_frames1; ++frame_idx) {
        int w = opts.width;
        int h = opts.height;
        int bd = opts.bit_depth;

        if (!chk_frames.empty() && frame_idx < (int)chk_frames.size()) {
            w = chk_frames[frame_idx].width;
            h = chk_frames[frame_idx].height;
            bd = chk_frames[frame_idx].bit_depth;
        }

        int sx, sy;
        chroma_shifts(opts.chroma, sx, sy);
        int uv_w = w >> sx;
        int uv_h = h >> sy;
        int bps = (bd > 8) ? 2 : 1;
        long long fsz = (w * h + 2 * uv_w * uv_h) * bps;
        long long target_off = (!frame_offsets.empty() && frame_idx < (int)frame_offsets.size())
                                   ? frame_offsets[frame_idx]
                                   : (long long)frame_idx * fsz;

        std::vector<uint8_t> buf1(fsz);
        f1.seekg(target_off);
        f1.read(reinterpret_cast<char*>(buf1.data()), fsz);

        if (f1.gcount() < fsz) {
            std::cerr << "Frame " << frame_idx << ": Read truncated (file ended early).\n";
            break;
        }

        std::cout << "\n=== Frame " << frame_idx << " [" << w << "x" << h << " @ " << bd << "-bit, Chroma " << opts.chroma << "] ===\n";

        // Pixel Inspector
        if (opts.inspect_x >= 0 && opts.inspect_y >= 0) {
            if (opts.inspect_x < w && opts.inspect_y < h) {
                int px = opts.inspect_x;
                int py = opts.inspect_y;
                int uvx = px >> sx;
                int uvy = py >> sy;
                int uvsz = uv_w * uv_h;

                int Y = 0, U = 0, V = 0;
                if (bd == 8) {
                    Y = buf1[py * w + px];
                    U = buf1[w * h + uvy * uv_w + uvx];
                    V = buf1[w * h + uvsz + uvy * uv_w + uvx];
                } else {
                    const auto *p16 = reinterpret_cast<const uint16_t*>(buf1.data());
                    Y = p16[py * w + px];
                    U = p16[w * h + uvy * uv_w + uvx];
                    V = p16[w * h + uvsz + uvy * uv_w + uvx];
                }

                int shift = (bd > 8) ? 2 : 0;
                RGB rgb = yuv_to_rgb((uint8_t)(Y >> shift), (uint8_t)(U >> shift), (uint8_t)(V >> shift));
                std::cout << "Pixel (" << px << ", " << py << "): Y=" << Y << " U=" << U << " V=" << V
                          << " -> RGB(" << (int)rgb.r << ", " << (int)rgb.g << ", " << (int)rgb.b << ")\n";
            } else {
                std::cerr << "Pixel coordinates (" << opts.inspect_x << ", " << opts.inspect_y << ") out of bounds for " << w << "x" << h << "\n";
            }
        }

        // Search YUV values
        if (opts.search_y >= 0 || opts.search_u >= 0 || opts.search_v >= 0) {
            int match_cnt = 0;
            int uvsz = uv_w * uv_h;
            const auto *p16 = reinterpret_cast<const uint16_t*>(buf1.data());

            for (int py = 0; py < h; ++py) {
                int uvy = py >> sy;
                for (int px = 0; px < w; ++px) {
                    int uvx = px >> sx;
                    int Y = (bd == 8) ? buf1[py * w + px] : p16[py * w + px];
                    int U = (bd == 8) ? buf1[w * h + uvy * uv_w + uvx] : p16[w * h + uvy * uv_w + uvx];
                    int V = (bd == 8) ? buf1[w * h + uvsz + uvy * uv_w + uvx] : p16[w * h + uvsz + uvy * uv_w + uvx];

                    if ((opts.search_y < 0 || Y == opts.search_y) &&
                        (opts.search_u < 0 || U == opts.search_u) &&
                        (opts.search_v < 0 || V == opts.search_v)) {
                        if (match_cnt < 20) {
                            std::cout << "Match at (" << px << ", " << py << "): Y=" << Y << " U=" << U << " V=" << V << "\n";
                        }
                        match_cnt++;
                    }
                }
            }
            std::cout << "Total YUV matches in Frame " << frame_idx << ": " << match_cnt << "\n";
        }

        // Frame Statistics Mode
        if (opts.stats_mode) {
            int uvsz = uv_w * uv_h;
            const auto *p16 = reinterpret_cast<const uint16_t*>(buf1.data());
            long long sum_y = 0, sum_u = 0, sum_v = 0;
            int min_y = 65535, max_y = 0;
            int min_u = 65535, max_u = 0;
            int min_v = 65535, max_v = 0;

            for (int py = 0; py < h; ++py) {
                int uvy = py >> sy;
                for (int px = 0; px < w; ++px) {
                    int uvx = px >> sx;
                    int Y = (bd == 8) ? buf1[py * w + px] : p16[py * w + px];
                    int U = (bd == 8) ? buf1[w * h + uvy * uv_w + uvx] : p16[w * h + uvy * uv_w + uvx];
                    int V = (bd == 8) ? buf1[w * h + uvsz + uvy * uv_w + uvx] : p16[w * h + uvsz + uvy * uv_w + uvx];

                    sum_y += Y; sum_u += U; sum_v += V;
                    min_y = std::min(min_y, Y); max_y = std::max(max_y, Y);
                    min_u = std::min(min_u, U); max_u = std::max(max_u, U);
                    min_v = std::min(min_v, V); max_v = std::max(max_v, V);
                }
            }
            long num_pixels = w * h;
            std::cout << "Frame Stats:\n"
                      << "  Y Plane: Min=" << min_y << ", Max=" << max_y << ", Avg=" << std::fixed << std::setprecision(2) << (double)sum_y / num_pixels << "\n"
                      << "  U Plane: Min=" << min_u << ", Max=" << max_u << ", Avg=" << (double)sum_u / num_pixels << "\n"
                      << "  V Plane: Min=" << min_v << ", Max=" << max_v << ", Avg=" << (double)sum_v / num_pixels << "\n";
        }

        // Diff Mode (Plane level + Pixel (x,y) Coordinate Diff)
        if (opts.diff_mode && frame_idx < total_frames2) {
            std::vector<uint8_t> buf2(fsz);
            f2.seekg(target_off);
            f2.read(reinterpret_cast<char*>(buf2.data()), fsz);

            if (f2.gcount() == fsz) {
                int y_bytes = w * h * bps;
                int uv_bytes = uv_w * uv_h * bps;

                bool y_same = (std::memcmp(buf1.data(), buf2.data(), y_bytes) == 0);
                bool u_same = (std::memcmp(buf1.data() + y_bytes, buf2.data() + y_bytes, uv_bytes) == 0);
                bool v_same = (std::memcmp(buf1.data() + y_bytes + uv_bytes, buf2.data() + y_bytes + uv_bytes, uv_bytes) == 0);

                std::cout << "Diff Comparison with Input 2:\n";
                std::cout << "  Y-plane: " << (y_same ? "IDENTICAL" : "DIFFERENT") << "\n";
                std::cout << "  U-plane: " << (u_same ? "IDENTICAL" : "DIFFERENT") << "\n";
                std::cout << "  V-plane: " << (v_same ? "IDENTICAL" : "DIFFERENT") << "\n";

                if (y_same && u_same && v_same) {
                    std::cout << "  --> Overall status: ALL PLANES IDENTICAL\n";
                } else {
                    // Pixel (x,y) Coordinate Diff Inspection
                    int diff_count = 0;
                    int uvsz = uv_w * uv_h;
                    const auto *p16_1 = reinterpret_cast<const uint16_t*>(buf1.data());
                    const auto *p16_2 = reinterpret_cast<const uint16_t*>(buf2.data());

                    std::cout << "  Differing Pixel (x,y) Coordinates:\n";

                    for (int py = 0; py < h; ++py) {
                        int uvy = py >> sy;
                        for (int px = 0; px < w; ++px) {
                            int uvx = px >> sx;

                            int Y1 = (bd == 8) ? buf1[py * w + px] : p16_1[py * w + px];
                            int U1 = (bd == 8) ? buf1[w * h + uvy * uv_w + uvx] : p16_1[w * h + uvy * uv_w + uvx];
                            int V1 = (bd == 8) ? buf1[w * h + uvsz + uvy * uv_w + uvx] : p16_1[w * h + uvsz + uvy * uv_w + uvx];

                            int Y2 = (bd == 8) ? buf2[py * w + px] : p16_2[py * w + px];
                            int U2 = (bd == 8) ? buf2[w * h + uvy * uv_w + uvx] : p16_2[w * h + uvy * uv_w + uvx];
                            int V2 = (bd == 8) ? buf2[w * h + uvsz + uvy * uv_w + uvx] : p16_2[w * h + uvsz + uvy * uv_w + uvx];

                            if (Y1 != Y2 || U1 != U2 || V1 != V2) {
                                diff_count++;
                                if (opts.max_diff_matches == 0 || diff_count <= opts.max_diff_matches) {
                                    std::cout << "    Diff at (" << px << ", " << py << "): "
                                              << "File1(Y=" << Y1 << ",U=" << U1 << ",V=" << V1 << ") vs "
                                              << "File2(Y=" << Y2 << ",U=" << U2 << ",V=" << V2 << ")\n";
                                }
                            }
                        }
                    }

                    std::cout << "  Total Differing Pixels in Frame " << frame_idx << ": " << diff_count << "\n";
                    if (opts.max_diff_matches > 0 && diff_count > opts.max_diff_matches) {
                        std::cout << "  (Output truncated to first " << opts.max_diff_matches
                                  << " matches. Use --max-diff <num> or --max-diff 0 for full list)\n";
                    }
                }
            }
        }

        // Export Frame to PPM
        if (!opts.export_ppm_prefix.empty()) {
            std::vector<uint8_t> rgb_buf(w * h * 3);
            int uvsz = uv_w * uv_h;
            int shift = (bd > 8) ? 2 : 0;
            const auto *p16 = reinterpret_cast<const uint16_t*>(buf1.data());

            for (int py = 0; py < h; ++py) {
                int uvy = py >> sy;
                for (int px = 0; px < w; ++px) {
                    int uvx = px >> sx;
                    int Y = (bd == 8) ? buf1[py * w + px] : p16[py * w + px];
                    int U = (bd == 8) ? buf1[w * h + uvy * uv_w + uvx] : p16[w * h + uvy * uv_w + uvx];
                    int V = (bd == 8) ? buf1[w * h + uvsz + uvy * uv_w + uvx] : p16[w * h + uvsz + uvy * uv_w + uvx];

                    RGB rgb = yuv_to_rgb((uint8_t)(Y >> shift), (uint8_t)(U >> shift), (uint8_t)(V >> shift));
                    int rgb_idx = (py * w + px) * 3;
                    rgb_buf[rgb_idx] = rgb.r;
                    rgb_buf[rgb_idx + 1] = rgb.g;
                    rgb_buf[rgb_idx + 2] = rgb.b;
                }
            }

            std::string ppm_name = opts.export_ppm_prefix + "_frame_" + std::to_string(frame_idx) + ".ppm";
            if (save_ppm(ppm_name, rgb_buf, w, h)) {
                std::cout << "Exported RGB frame to " << ppm_name << "\n";
            }
        }
    }

    return 0;
}
