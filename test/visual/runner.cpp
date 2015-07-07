/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2015 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

// stl
#include <algorithm>
#include <future>

#include <mapnik/load_map.hpp>

#include "runner.hpp"

namespace visual_tests
{

class renderer_visitor
{
public:
    renderer_visitor(std::string const & name,
                     mapnik::Map & map,
                     map_size const & tiles,
                     double scale_factor,
                     result_list & results,
                     report_type & report)
        : name_(name),
          map_(map),
          tiles_(tiles),
          scale_factor_(scale_factor),
          results_(results),
          report_(report)
    {
    }

    template <typename T, typename std::enable_if<T::renderer_type::support_tiles>::type* = nullptr>
    void operator()(T const & renderer)
    {
        result r;
        if (tiles_.width == 1 && tiles_.height == 1)
        {
            r = renderer.test(name_, map_, scale_factor_);
        }
        else
        {
            r = renderer.test_tiles(name_, map_, tiles_, scale_factor_);
        }
        mapnik::util::apply_visitor(report_visitor(r), report_);
        results_.push_back(std::move(r));
    }

    template <typename T, typename std::enable_if<!T::renderer_type::support_tiles>::type* = nullptr>
    void operator()(T const & renderer)
    {
        if (tiles_.width == 1 && tiles_.height == 1)
        {
            result r = renderer.test(name_, map_, scale_factor_);
            mapnik::util::apply_visitor(report_visitor(r), report_);
            results_.push_back(std::move(r));
        }
    }

private:
    std::string const & name_;
    mapnik::Map & map_;
    map_size const & tiles_;
    double scale_factor_;
    result_list & results_;
    report_type & report_;
};

runner::runner(runner::path_type const & styles_dir,
               runner::path_type const & output_dir,
               runner::path_type const & reference_dir,
               bool overwrite,
               std::size_t jobs)
    : styles_dir_(styles_dir),
      output_dir_(output_dir),
      reference_dir_(reference_dir),
      jobs_(jobs),
      renderers_{ renderer<agg_renderer>(output_dir_, reference_dir_, overwrite)
#if defined(HAVE_CAIRO)
                  ,renderer<cairo_renderer>(output_dir_, reference_dir_, overwrite)
#endif
#if defined(SVG_RENDERER)
                  ,renderer<svg_renderer>(output_dir_, reference_dir_, overwrite)
#endif
#if defined(GRID_RENDERER)
                  ,renderer<grid_renderer>(output_dir_, reference_dir_, overwrite)
#endif
                }
{
}

result_list runner::test_all(report_type & report) const
{
    boost::filesystem::directory_iterator begin(styles_dir_);
    boost::filesystem::directory_iterator end;
    std::vector<runner::path_type> files(begin, end);
    return test_parallel(files, report, jobs_);
}

result_list runner::test(std::vector<std::string> const & style_names, report_type & report) const
{
    std::vector<runner::path_type> files(style_names.size());
    std::transform(style_names.begin(), style_names.end(), std::back_inserter(files),
        [&](runner::path_type const & name)
        {
            return (name.extension() == ".xml") ? name : (styles_dir_ / (name.string() + ".xml"));
        });
    return test_parallel(files, report, jobs_);
}

result_list runner::test_parallel(std::vector<runner::path_type> const & files, report_type & report, std::size_t jobs) const
{
    result_list results;

    if (files.empty())
    {
        return results;
    }

    if (jobs == 0)
    {
        jobs = 1;
    }

    std::size_t chunk_size = files.size() / jobs;

    if (chunk_size == 0)
    {
        chunk_size = files.size();
        jobs = 1;
    }

    std::launch launch(jobs == 1 ? std::launch::deferred : std::launch::async);
    std::vector<std::future<result_list>> futures(jobs);

    for (std::size_t i = 0; i < jobs; i++)
    {
        files_iterator begin(files.begin() + i * chunk_size);
        files_iterator end(files.begin() + (i + 1) * chunk_size);

        // Handle remainder of files.size() / jobs
        if (i == jobs - 1)
        {
            end = files.end();
        }

        futures[i] = std::async(launch, &runner::test_range, this, begin, end, std::ref(report));
    }

    for (auto & f : futures)
    {
        result_list r = f.get();
        std::move(r.begin(), r.end(), std::back_inserter(results));
    }

    return results;
}

result_list runner::test_range(files_iterator begin, files_iterator end, std::reference_wrapper<report_type> report) const
{
    config defaults;
    result_list results;

    for (runner::files_iterator i = begin; i != end; i++)
    {
        runner::path_type const & file = *i;
        if (file.extension() == ".xml")
        {
            try
            {
                result_list r = test_one(file, defaults, report);
                std::move(r.begin(), r.end(), std::back_inserter(results));
            }
            catch (std::exception const& ex)
            {
                result r;
                r.state = STATE_ERROR;
                r.name = file.string();
                r.error_message = ex.what();
                results.emplace_back(r);
                mapnik::util::apply_visitor(report_visitor(r), report.get());
            }
        }
    }

    return results;
}

result_list runner::test_one(runner::path_type const& style_path, config cfg, report_type & report) const
{
    mapnik::Map map(cfg.sizes.front().width, cfg.sizes.front().height);
    result_list results;

    try
    {
        mapnik::load_map(map, style_path.string(), true);
    }
    catch (std::exception const& ex)
    {
        std::string what = ex.what();
        if (what.find("Could not create datasource") != std::string::npos ||
            what.find("Postgis Plugin: could not connect to server") != std::string::npos)
        {
            return results;
        }
        throw;
    }

    mapnik::parameters const & params = map.get_extra_parameters();

    boost::optional<mapnik::value_integer> status = params.get<mapnik::value_integer>("status", cfg.status);

    if (!*status)
    {
        return results;
    }

    boost::optional<std::string> sizes = params.get<std::string>("sizes");

    if (sizes)
    {
        cfg.sizes.clear();
        parse_map_sizes(*sizes, cfg.sizes);
    }

    boost::optional<std::string> tiles = params.get<std::string>("tiles");

    if (tiles)
    {
        cfg.tiles.clear();
        parse_map_sizes(*tiles, cfg.tiles);
    }

    boost::optional<std::string> bbox_string = params.get<std::string>("bbox");
    mapnik::box2d<double> box;

    if (bbox_string)
    {
        box.from_string(*bbox_string);
    }

    std::string name(style_path.stem().string());

    for (auto const & size : cfg.sizes)
    {
        for (auto const & scale_factor : cfg.scales)
        {
            for (auto const & tiles_count : cfg.tiles)
            {
                if (!tiles_count.width || !tiles_count.height)
                {
                    throw std::runtime_error("Cannot render zero tiles.");
                }
                if (size.width % tiles_count.width || size.height % tiles_count.height)
                {
                    throw std::runtime_error("Tile size is not an integer.");
                }

                for (auto const & ren : renderers_)
                {
                    map.resize(size.width, size.height);
                    if (box.valid())
                    {
                        map.zoom_to_box(box);
                    }
                    else
                    {
                        map.zoom_all();
                    }
                    mapnik::util::apply_visitor(renderer_visitor(name, map, tiles_count, scale_factor, results, report), ren);
                }
            }
        }
    }

    return results;
}

void runner::parse_map_sizes(std::string const & str, std::vector<map_size> & sizes) const
{
    boost::spirit::ascii::space_type space;
    std::string::const_iterator iter = str.begin();
    std::string::const_iterator end = str.end();
    if (!boost::spirit::qi::phrase_parse(iter, end, map_sizes_parser_, space, sizes))
    {
        throw std::runtime_error("Failed to parse list of sizes: '" + str + "'");
    }
}

}

