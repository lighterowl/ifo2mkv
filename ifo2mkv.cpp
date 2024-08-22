/*
 Modified from original mkvtoolnix code by Daniel Kamil Kozar <dkk089@gmail.com>
 Distributed under the GPL v2

 This is pretty much code taken 1:1 from mkvtoolnix/src/common/chapters/dvd.cpp
 I only made it runnable as a standalone application so really shouldn't take
 any credit for it.

 Original license statement from said file is reproduced below :

 Distributed under the GPL v2
 see the file COPYING for details
 or visit https://www.gnu.org/licenses/old-licenses/gpl-2.0.html

 helper functions for chapters on DVDs

 Written by Moritz Bunkus <moritz@bunkus.org>.
 */

#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

namespace
{
struct libdvdread_logger : public dvd_logger_cb
{
  libdvdread_logger()
  {
    this->pf_log = pf_log_;
  }
  ~libdvdread_logger()
  {
    if (do_report_messages_)
    {
      std::cerr << "Messages reported by libdvdread :\n";
      for (auto &&msg : messages_)
      {
        std::cerr << std::format("[{}] {}\n", lvl_to_str(msg.first), msg.second);
      }
    }
  }

  void disable_report()
  {
    do_report_messages_ = false;
  }

private:
  static std::string_view lvl_to_str(dvd_logger_level_t lvl)
  {
    switch (lvl)
    {
    case DVD_LOGGER_LEVEL_INFO:
      return "INFO";
    case DVD_LOGGER_LEVEL_ERROR:
      return "ERROR";
    case DVD_LOGGER_LEVEL_WARN:
      return "WARN";
    case DVD_LOGGER_LEVEL_DEBUG:
      return "DEBUG";
    default:
      return "unknown";
    }
  }

  static void pf_log_(void *p, dvd_logger_level_t lvl, char const *fmt, va_list args)
  {
    char msg[16 * 1024];
    auto const num_written = ::vsnprintf(msg, sizeof(msg), fmt, args);
    if (num_written >= 0)
    {
      auto const sv_length =
          static_cast<std::string_view::size_type>(std::min(num_written, static_cast<int>(sizeof(msg) - 1)));
      static_cast<libdvdread_logger *>(p)->real_log(lvl, std::string_view{msg, sv_length});
    }
  }
  void real_log(dvd_logger_level_t lvl, std::string_view str)
  {
    messages_.emplace_back(std::make_pair(lvl, str));
  }

  std::vector<std::pair<dvd_logger_level_t, std::string>> messages_;
  bool do_report_messages_ = true;
};

struct libdvdread_exception : public std::runtime_error
{
  libdvdread_exception(std::string const &what) : runtime_error(what)
  {
  }
};

using dvd_uptr = std::unique_ptr<dvd_reader_t, decltype(&::DVDClose)>;
auto dvd_open(char const *path, libdvdread_logger &logger)
{
  if (auto const dvd = ::DVDOpen2(&logger, &logger, path))
  {
    return dvd_uptr{dvd, [](auto p) {
                      if (p)
                      {
                        ::DVDClose(p);
                      }
                    }};
  }
  else
  {
    throw libdvdread_exception(std::format("Failed to open DVD structure under {}", path));
  }
}

using ifo_uptr = std::unique_ptr<ifo_handle_t, decltype(&::ifoClose)>;
auto ifo_open(dvd_reader_t &dvd, int title)
{
  if (auto const ifo = ::ifoOpen(&dvd, title))
  {
    return ifo_uptr{ifo, [](auto p) {
                      if (p)
                      {
                        ::ifoClose(p);
                      }
                    }};
  }
  else
  {
    throw libdvdread_exception(std::format("Failed to open IFO for title {}", title));
  }
}

struct matroska_chapter_xml_writer
{
  matroska_chapter_xml_writer(std::ostream &stream) : rnd_gen_(std::random_device{}()), stream_(stream)
  {
    stream_ << R"(<?xml version="1.0"?>
<!-- <!DOCTYPE Chapters SYSTEM "matroskachapters.dtd"> -->
<Chapters>
)";
  }

  ~matroska_chapter_xml_writer()
  {
    stream_ << "</Chapters>\n";
  }

  void on_title_start()
  {
    stream_ << std::format(R"(  <EditionEntry>
    <EditionFlagHidden>0</EditionFlagHidden>
    <EditionFlagDefault>0</EditionFlagDefault>
    <EditionFlagOrdered>0</EditionFlagOrdered>
    <EditionUID>{}</EditionUID>
)",
                           rnd_gen_());
    chapter_num_ = 1;
  }
  void on_title_end()
  {
    stream_ << "  </EditionEntry>\n";
  }
  void on_chapter_start(int32_t timestamp_ms)
  {
    auto const hr = timestamp_ms / 3600000;
    auto const min = (timestamp_ms / 60000) % 60;
    auto const sec = (timestamp_ms / 1000) % 60;
    auto const ms = timestamp_ms % 1000;
    stream_ << std::format(R"(    <ChapterAtom>
      <ChapterUID>{}</ChapterUID>
      <ChapterTimeStart>{:02}:{:02}:{:02}.{:03}</ChapterTimeStart>
      <ChapterDisplay>
        <ChapterString>Chapter {:02}</ChapterString>
        <ChapterLanguage>und</ChapterLanguage>
        <ChapLanguageIETF>und</ChapLanguageIETF>
      </ChapterDisplay>
    </ChapterAtom>
)",
                           rnd_gen_(), hr, min, sec, ms, chapter_num_++);
  }

private:
  std::mt19937_64 rnd_gen_;
  std::ostream &stream_;
  unsigned chapter_num_ = 1;
};

int32_t frames_to_timestamp_ms(unsigned int num_frames, unsigned int fps)
{
  auto factor = fps == 30 ? 1001 : 1000;
  return static_cast<int32_t>(factor * num_frames / (fps ? fps : 1));
}

template <typename T> constexpr T from_bcd(T val)
{
  return (((val & 0xf0) >> 4) * 10) + (val & 0x0f);
}

template <typename Writer> void get_chapters_for_title(dvd_reader_t &dvd, ifo_handle_t &vmg, int title, Writer &writer)
{
  auto vts = ifo_open(dvd, vmg.tt_srpt->title[title].title_set_nr);

  writer.on_title_start();
  writer.on_chapter_start(0);

  auto ttn = vmg.tt_srpt->title[title].vts_ttn;
  auto vts_ptt_srpt = vts->vts_ptt_srpt;
  auto overall_frames = 0u;
  auto fps = 0u; // This should be consistent as DVDs are either NTSC or PAL

  for (auto chapter = 0; chapter < vmg.tt_srpt->title[title].nr_of_ptts - 1; chapter++)
  {
    auto pgc_id = vts_ptt_srpt->title[ttn - 1].ptt[chapter].pgcn;
    auto pgn = vts_ptt_srpt->title[ttn - 1].ptt[chapter].pgn;
    auto cur_pgc = vts->vts_pgcit->pgci_srp[pgc_id - 1].pgc;
    auto start_cell = cur_pgc->program_map[pgn - 1] - 1;
    pgc_id = vts_ptt_srpt->title[ttn - 1].ptt[chapter + 1].pgcn;
    pgn = vts_ptt_srpt->title[ttn - 1].ptt[chapter + 1].pgn;
    cur_pgc = vts->vts_pgcit->pgci_srp[pgc_id - 1].pgc;
    auto end_cell = cur_pgc->program_map[pgn - 1] - 2;
    auto cur_frames = 0u;

    for (auto cur_cell = start_cell; cur_cell <= end_cell; cur_cell++)
    {
      auto dt = &cur_pgc->cell_playback[cur_cell].playback_time;
      auto hour = from_bcd(dt->hour);
      auto minute = from_bcd(dt->minute);
      auto second = from_bcd(dt->second);
      fps = ((dt->frame_u & 0xc0) >> 6) == 1 ? 25 : 30; // by definition
      cur_frames += ((hour * 60 * 60) + minute * 60 + second) * fps;
      cur_frames += ((dt->frame_u & 0x30) >> 4) * 10 + (dt->frame_u & 0x0f);
    }

    overall_frames += cur_frames;
    writer.on_chapter_start(frames_to_timestamp_ms(overall_frames, fps));
  }

  writer.on_title_end();
}
} // namespace

int main(int argc, char **argv)
{
  if (!(argc == 2 || argc == 3))
  {
    std::cerr << "Usage : " << argv[0]
              << " path_to_VIDEO_TS [title_no]\n"
                 "If title_no is not specified or 0, chapters from all titles "
                 "are output\n";

    return 1;
  }

  auto title = unsigned{};
  if (argc == 3)
  {
    try
    {
      auto title_parsed = std::stoi(argv[2]);
      if (title_parsed < 0)
      {
        std::cerr << "Title cannot be a negative integer.\n";
        return 1;
      }
      title = static_cast<unsigned>(title_parsed);
    }
    catch (...)
    {
      std::cerr << "Could not convert " << argv[2] << " to integer\n";
      return 1;
    }
  }

  auto logger = libdvdread_logger{};
  try
  {
    auto dvd = dvd_open(argv[1], logger);
    auto vmg = ifo_open(*dvd, 0);

    if (title == 0u)
    {
      auto writer = matroska_chapter_xml_writer{std::cout};
      for (auto t = 0u; t < vmg->tt_srpt->nr_of_srpts; ++t)
      {
        get_chapters_for_title(*dvd, *vmg, t, writer);
      }
    }
    else
    {
      if (title >= vmg->tt_srpt->nr_of_srpts)
      {
        std::cerr << "Title " << title << " requested, but DVD has " << vmg->tt_srpt->nr_of_srpts << " titles.\n";
        return 1;
      }
      auto writer = matroska_chapter_xml_writer{std::cout};
      get_chapters_for_title(*dvd, *vmg, title, writer);
    }
  }
  catch (libdvdread_exception const &ex)
  {
    std::cerr << "DVD read error : " << ex.what() << '\n';
    return 1;
  }
  catch (std::exception const &ex)
  {
    std::cerr << "Fatal error : " << ex.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cerr << "Unknown error\n";
    return 1;
  }

  logger.disable_report();
}
