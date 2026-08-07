#include "types.h"
#include "render_grid.h"
#include "html_tag.h"
#include "document.h"

#include <algorithm>
#include <vector>

// Strip the overflow/baseline qualifier bits to get the base alignment value.
static int base_align(int a)
{
    return a & 0xFF;
}

int litehtml::render_item_grid::effective_self_align(const std::shared_ptr<render_item>& item, bool inline_axis)
{
    int flags;
    int base;
    if(inline_axis)
    {
        int v  = static_cast<int>(item->css().get_grid_justify_self());
        flags  = v & ~0xFF;
        base   = base_align(v);
        if(base == flex_align_items_auto) base = base_align(static_cast<int>(css().get_grid_justify_items()));
    } else
    {
        int v  = static_cast<int>(item->css().get_flex_align_self());
        flags  = v & ~0xFF;
        base   = base_align(v);
        if(base == flex_align_items_auto) base = base_align(static_cast<int>(css().get_flex_align_items()));
    }
    // On grid items `normal` behaves as `stretch` (except on boxes with an
    // intrinsic aspect ratio or intrinsic size, which we approximate as
    // stretch here too; replaced elements opt out of stretch in the caller).
    if(base == flex_align_items_auto || base == flex_align_items_normal) base = flex_align_items_stretch;
    return base | flags;
}

litehtml::pixel_t litehtml::render_item_grid::natural_item_width(const std::shared_ptr<render_item>& item,
                                                                 const containing_block_context& self_size,
                                                                 formatting_context*             fmt_ctx)
{
    pixel_t w = item->render(0_px, 0_px,
                             self_size.new_width(self_size.render_width, containing_block_context::size_mode_content),
                             fmt_ctx)
                    .natural_width;
    return w + item->content_offset_width();
}

void litehtml::render_item_grid::distribute_tracks(flex_justify_content dist, pixel_t container_size, pixel_t base_gap,
                                                   std::vector<pixel_t>& tracks, const std::vector<char>& is_auto,
                                                   pixel_t& out_offset, pixel_t& out_extra_gap)
{
    out_offset    = 0_px;
    out_extra_gap = 0_px;
    const int n   = static_cast<int>(tracks.size());
    if(n == 0) return;

    pixel_t total = n > 1 ? pixel_t(n - 1) * base_gap : 0_px;
    for(auto t : tracks) total += t;
    pixel_t free_space = container_size - total;
    if(free_space < 0_px) free_space = 0_px;

    switch(dist)
    {
    case flex_justify_content_end:
    case flex_justify_content_flex_end:
    case flex_justify_content_right:
        out_offset = free_space;
        break;
    case flex_justify_content_center:
        out_offset = free_space / 2_px;
        break;
    case flex_justify_content_space_between:
        if(n > 1) out_extra_gap = free_space / pixel_t(n - 1);
        break;
    case flex_justify_content_space_around:
        out_extra_gap = free_space / pixel_t(n);
        out_offset    = out_extra_gap / 2_px;
        break;
    case flex_justify_content_space_evenly:
        out_extra_gap = free_space / pixel_t(n + 1);
        out_offset    = out_extra_gap;
        break;
    case flex_justify_content_stretch:
    {
        // Grow the auto tracks to consume the free space (CSS Grid 12.4).
        int auto_count = 0;
        for(int i = 0; i < n; i++)
            if(is_auto[i]) auto_count++;
        if(auto_count > 0)
        {
            pixel_t add = free_space / pixel_t(auto_count);
            for(int i = 0; i < n; i++)
                if(is_auto[i]) tracks[i] += add;
        }
        break;
    }
    default: // normal / start / flex-start / left
        break;
    }
}

std::vector<litehtml::pixel_t> litehtml::render_item_grid::resolve_columns(const grid_track_vector& tracks,
                                                                           pixel_t available, bool available_definite,
                                                                           const std::vector<std::shared_ptr<render_item>>& items,
                                                                           const std::vector<grid_item_area>& areas, int ncols,
                                                                           const containing_block_context& self_size,
                                                                           formatting_context* fmt_ctx, pixel_t col_gap,
                                                                           std::vector<char>* out_is_auto)
{
    const auto doc   = src_el()->get_document();
    const auto& fmet = css().get_font_metrics();

    // Measure the max-content width of the items placed in a given column. Only
    // single-column-spanning items contribute directly; a multi-span item's size
    // distribution across tracks is approximated by attributing it to its first
    // column (spanning distribution is a known C19 simplification).
    auto measure_column = [&](int col) -> pixel_t {
        pixel_t max_w = 0_px;
        for(size_t i = 0; i < items.size(); i++)
        {
            if(areas[i].col_start != col) continue;
            const auto& el = items[i];
            pixel_t     w =
                el->render(0_px, 0_px, self_size.new_width(available, containing_block_context::size_mode_content),
                           fmt_ctx)
                    .natural_width;
            w += el->content_offset_width();
            if(w > max_w) max_w = w;
        }
        return max_w;
    };

    // `none` (empty track list) with no grown implicit columns -> a single auto
    // column holding every item. When placement grew the column count beyond the
    // explicit list, fall through to the general loop (implicit tracks are auto).
    if(tracks.empty() && ncols <= 1)
    {
        if(available_definite) return {available};
        return {measure_column(0)};
    }

    // The length a track sizes toward: the max bound for minmax(), else its value.
    // A track index beyond the explicit list is an implicit (auto) track grown by
    // definite placement.
    auto track_size = [&](int c) -> const css_length& {
        static const css_length implicit_auto = css_length::predef_value(0);
        if(c >= (int)tracks.size()) return implicit_auto;
        return tracks[c].is_minmax ? tracks[c].max : tracks[c].min;
    };

    std::vector<pixel_t> col_w(ncols, 0_px);
    std::vector<pixel_t> floor_px(ncols, 0_px); // minmax() min bound, resolved
    std::vector<bool>    is_auto(ncols, false);
    float                total_fr = 0.0f;

    for(int c = 0; c < ncols; c++)
    {
        // A minmax() track sizes toward its max bound; a plain track uses its value.
        const css_length& tr = track_size(c);
        if(c < (int)tracks.size() && tracks[c].is_minmax)
        {
            // Resolve the inflexible min bound into a floor. auto/min-content/
            // max-content minimums are not modelled (floored at 0); a percentage
            // minimum against an indefinite width also resolves to 0.
            const css_length& mn = tracks[c].min;
            if(!mn.is_predefined() && mn.units() != css_units_fr &&
               (mn.units() != css_units_percentage || available_definite))
            {
                floor_px[c] = doc->to_pixels(mn, fmet, available);
            }
        }
        if(tr.is_predefined())
        {
            // auto / min-content / max-content -> content sized (measured below).
            is_auto[c] = true;
        } else if(tr.units() == css_units_fr)
        {
            if(available_definite)
            {
                total_fr += tr.val();
            } else
            {
                // fr in an indefinite axis is content sized.
                is_auto[c] = true;
            }
        } else if(tr.units() == css_units_percentage && !available_definite)
        {
            // percentage against an indefinite width behaves as auto.
            is_auto[c] = true;
        } else
        {
            // Absolute unit, or percentage against a definite content width.
            col_w[c] = doc->to_pixels(tr, fmet, available);
        }
    }

    // Distribute the remaining free space across fr tracks (definite width only).
    // The gaps between columns consume container width too, so they are removed
    // from the free space before sizing fr tracks.
    if(total_fr > 0.0f)
    {
        pixel_t used = 0_px;
        for(int c = 0; c < ncols; c++)
        {
            if(!is_auto[c] && track_size(c).units() != css_units_fr) used += col_w[c];
        }
        if(ncols > 1) used += pixel_t(ncols - 1) * col_gap;
        pixel_t free_space = available - used;
        if(free_space < 0_px) free_space = 0_px;
        for(int c = 0; c < ncols; c++)
        {
            if(!is_auto[c] && track_size(c).units() == css_units_fr)
            {
                col_w[c] = free_space * (track_size(c).val() / total_fr);
            }
        }
    }

    // Content-size the auto (and indefinite-%/fr) columns from their items.
    for(int c = 0; c < ncols; c++)
    {
        if(is_auto[c]) col_w[c] = measure_column(c);
    }

    // Clamp each minmax() track up to its resolved min bound.
    for(int c = 0; c < ncols; c++)
    {
        if(col_w[c] < floor_px[c]) col_w[c] = floor_px[c];
    }

    if(out_is_auto)
    {
        out_is_auto->assign(is_auto.begin(), is_auto.end());
    }

    return col_w;
}

int litehtml::render_item_grid::resolve_axis(const grid_line& gstart, const grid_line& gend, int explicit_count,
                                             int& out_span)
{
    // Normalize a 1-based line number; a negative line counts back from the line
    // after the last explicit track (line -1 == explicit_count + 1).
    auto abs_line = [&](int line) -> int {
        if(line < 0) return (explicit_count + 1) + line + 1;
        return line;
    };

    const bool s_line = !gstart.is_auto && gstart.line != 0;
    const bool e_line = !gend.is_auto && gend.line != 0;
    const int  s_span = (!gstart.is_auto) ? gstart.span : 0;
    const int  e_span = (!gend.is_auto) ? gend.span : 0;

    int span = 1;
    if(e_span > 0) span = e_span;
    else if(s_span > 0) span = s_span;
    else if(s_line && e_line)
    {
        span = abs_line(gend.line) - abs_line(gstart.line);
    }
    if(span < 1) span = 1;
    out_span = span;

    if(s_line) return abs_line(gstart.line) - 1;              // 0-based start track
    if(e_line) return (abs_line(gend.line) - 1) - (span - 1); // end track, span tracks back
    return -1;                                                // auto start
}

std::vector<litehtml::render_item_grid::grid_item_area>
litehtml::render_item_grid::place_items(const std::vector<std::shared_ptr<render_item>>& items, int explicit_cols,
                                        int explicit_rows, int& out_ncols, int& out_nrows)
{
    const bool col_flow = (css().get_grid_auto_flow() == grid_auto_flow_column);
    const bool dense    = css().get_grid_auto_flow_dense();
    const int  n        = static_cast<int>(items.size());

    std::vector<int> col_start(n), col_span(n, 1), row_start(n), row_span(n, 1);
    int              max_col = explicit_cols > 0 ? explicit_cols : 1;
    int              max_row = explicit_rows > 0 ? explicit_rows : 0;
    for(int i = 0; i < n; i++)
    {
        const auto& c  = items[i]->src_el()->css();
        col_start[i]   = resolve_axis(c.get_grid_column_start(), c.get_grid_column_end(), explicit_cols, col_span[i]);
        row_start[i]   = resolve_axis(c.get_grid_row_start(), c.get_grid_row_end(), explicit_rows, row_span[i]);
        if(col_start[i] >= 0) max_col = (std::max)(max_col, col_start[i] + col_span[i]);
        if(row_start[i] >= 0) max_row = (std::max)(max_row, row_start[i] + row_span[i]);
    }
    // In column flow the row count is the fixed axis; default to the explicit
    // rows (or a single row when none) and let columns grow implicitly.
    const int ncols = col_flow ? (explicit_cols > 0 ? explicit_cols : 1) : max_col;
    const int fixed_rows = col_flow ? (explicit_rows > 0 ? explicit_rows : 1) : 0;

    std::vector<grid_item_area> areas(n);

    // Occupancy grid; rows (and columns in column flow) grow on demand.
    std::vector<std::vector<char>> occ;
    auto ensure = [&](int r, int c) {
        if(r >= static_cast<int>(occ.size())) occ.resize(r + 1);
        for(auto& row : occ)
            if(c >= static_cast<int>(row.size())) row.resize(c + 1, 0);
    };
    auto fits = [&](int r, int c, int rs, int cs) -> bool {
        if(r < 0 || c < 0) return false;
        if(!col_flow && c + cs > ncols) return false;      // row flow: columns are bounded
        if(col_flow && fixed_rows > 0 && r + rs > fixed_rows) return false; // column flow: rows bounded
        if(r >= static_cast<int>(occ.size())) return true; // beyond placed rows = empty
        for(int rr = r; rr < r + rs; rr++)
            for(int cc = c; cc < c + cs; cc++)
                if(rr < static_cast<int>(occ.size()) && cc < static_cast<int>(occ[rr].size()) && occ[rr][cc])
                    return false;
        return true;
    };
    auto mark = [&](int r, int c, int rs, int cs) {
        ensure(r + rs - 1, c + cs - 1);
        for(int rr = r; rr < r + rs; rr++)
            for(int cc = c; cc < c + cs; cc++) occ[rr][cc] = 1;
    };

    // Phase 1: both axes definite.
    for(int i = 0; i < n; i++)
        if(col_start[i] >= 0 && row_start[i] >= 0)
        {
            areas[i] = {col_start[i], col_span[i], row_start[i], row_span[i]};
            mark(row_start[i], col_start[i], row_span[i], col_span[i]);
        }
    // Phase 2: definite row, auto column — scan that row for a free column span.
    for(int i = 0; i < n; i++)
        if(row_start[i] >= 0 && col_start[i] < 0)
        {
            int c = 0;
            while(!fits(row_start[i], c, row_span[i], col_span[i]))
            {
                c++;
                // Row flow bounds the columns: an over-wide span that no column
                // position fits is placed at the row origin (overlap tolerated).
                if(!col_flow && c + col_span[i] > ncols) { c = 0; break; }
            }
            areas[i] = {c, col_span[i], row_start[i], row_span[i]};
            mark(row_start[i], c, row_span[i], col_span[i]);
        }
    // Phase 3: the rest (auto row). Cursor advances per auto-flow.
    int cur_r = 0, cur_c = 0;
    for(int i = 0; i < n; i++)
    {
        if(row_start[i] >= 0) continue; // already placed
        const int cs = col_span[i], rs = row_span[i];
        if(col_start[i] >= 0)
        {
            // Definite column, auto row: scan down that column from the top.
            int c = col_start[i], r = 0;
            while(!fits(r, c, rs, cs))
            {
                r++;
                // Column flow bounds the rows: an over-tall span that no row
                // position fits is placed at the column origin (overlap tolerated).
                if(col_flow && fixed_rows > 0 && r + rs > fixed_rows) { r = 0; break; }
            }
            areas[i] = {c, cs, r, rs};
            mark(r, c, rs, cs);
            continue;
        }
        // Fully auto. Dense repacks from the origin; sparse keeps moving forward.
        if(dense)
        {
            cur_r = 0;
            cur_c = 0;
        }
        int r, c;
        for(;;)
        {
            r = cur_r;
            c = cur_c;
            if(!col_flow && c + cs > ncols)
            {
                cur_c = 0;
                cur_r++;
                continue;
            }
            if(col_flow && fixed_rows > 0 && r + rs > fixed_rows)
            {
                cur_r = 0;
                cur_c++;
                continue;
            }
            if(fits(r, c, rs, cs)) break;
            if(col_flow) cur_r++;
            else cur_c++;
        }
        areas[i] = {c, cs, r, rs};
        mark(r, c, rs, cs);
        if(col_flow) cur_r = r + rs;
        else cur_c = c + cs;
    }

    int nrows = max_row;
    int ncols_final = ncols;
    for(int i = 0; i < n; i++)
    {
        nrows       = (std::max)(nrows, areas[i].row_start + areas[i].row_span);
        ncols_final = (std::max)(ncols_final, areas[i].col_start + areas[i].col_span);
    }
    out_ncols = ncols_final;
    out_nrows = nrows;
    return areas;
}

litehtml::rendered_width litehtml::render_item_grid::_render_content(pixel_t x, pixel_t y, bool /*second_pass*/,
                                                                     const containing_block_context& self_size,
                                                                     formatting_context*             fmt_ctx)
{
    // A grid container is shrink-to-fit when it is inline-level (inline-grid),
    // floated or absolutely positioned: its columns are then content sized and
    // the natural width is their sum. A block-level grid in normal flow has a
    // definite width (it fills its containing block) even when width is auto.
    const bool shrink_to_fit = (css().get_display() == display_inline_grid) ||
                               (src_el()->css().get_float() != float_none) ||
                               (src_el()->css().get_position() == element_position_absolute) ||
                               (src_el()->css().get_position() == element_position_fixed);
    const bool    width_definite = !shrink_to_fit;
    const pixel_t content_width  = self_size.render_width;
    pixel_t       ret_width      = content_width;

    // Collect in-flow items (document order == auto-placement order).
    std::vector<std::shared_ptr<render_item>> items;
    for(const auto& el : m_children)
    {
        auto disp = el->src_el()->css().get_display();
        auto pos  = el->src_el()->css().get_position();
        if(disp == display_none) continue;
        if(pos == element_position_absolute || pos == element_position_fixed) continue;
        items.push_back(el);
    }

    if(items.empty())
    {
        pixel_t empty_w = width_definite ? content_width : 0_px;
        m_pos.width     = empty_w;
        m_pos.height    = 0_px;
        return {empty_w, empty_w};
    }

    const grid_track_vector& cols_t = css().get_grid_template_columns();
    const grid_track_vector& rows_t = css().get_grid_template_rows();

    // column-gap resolves its percentage against the container's inline size
    // (content-box width). row-gap (below) resolves against the block size.
    const pixel_t col_gap = css().get_column_gap().calc_percent(content_width);

    // Place every in-flow item into a concrete grid area: explicit line-based
    // placement first, then auto-flow for the rest. Grows implicit tracks and
    // yields the final column/row counts.
    int                         ncols = 0, nrows = 0;
    std::vector<grid_item_area> areas = place_items(items, static_cast<int>(cols_t.size()),
                                                    static_cast<int>(rows_t.size()), ncols, nrows);

    std::vector<char>    col_is_auto;
    std::vector<pixel_t> col_w = resolve_columns(cols_t, content_width, width_definite, items, areas,
                                                 ncols, self_size, fmt_ctx, col_gap, &col_is_auto);

    // Two-phase percentage resolution for an indefinite (shrink-to-fit) axis.
    // Phase 1 above treated % tracks as auto to get the intrinsic width. If any
    // explicit column is a percentage, phase 2 re-resolves those tracks against
    // that intrinsic width (CSS Grid 5.1.1: % against an indefinite size first
    // computes the intrinsic size, then resolves the percentage against it).
    if(!width_definite)
    {
        bool has_pct_col = false;
        for(const auto& t : cols_t)
        {
            const css_length& eff = t.is_minmax ? t.max : t.min;
            if(!eff.is_predefined() && eff.units() == css_units_percentage) { has_pct_col = true; break; }
        }
        if(has_pct_col)
        {
            pixel_t intrinsic = 0_px;
            for(int c = 0; c < ncols; c++) intrinsic += col_w[c];
            if(ncols > 1) intrinsic += pixel_t(ncols - 1) * col_gap;
            col_w = resolve_columns(cols_t, intrinsic, /*available_definite=*/true, items, areas,
                                    ncols, self_size, fmt_ctx, col_gap, &col_is_auto);
        }
    }

    // justify-content distributes the free inline space among/around the tracks
    // (only meaningful with a definite container width; otherwise the container
    // shrink-wraps the tracks and there is no free space).
    pixel_t jc_off = 0_px, jc_extra = 0_px;
    if(width_definite)
    {
        distribute_tracks(css().get_flex_justify_content(), content_width, col_gap, col_w, col_is_auto, jc_off,
                          jc_extra);
    }

    // The grid's content-box width: the definite containing-block width, or the
    // sum of the content-sized columns (plus the gaps between them) when
    // shrink-to-fit.
    pixel_t grid_width = content_width;
    if(!width_definite)
    {
        grid_width = 0_px;
        for(int c = 0; c < ncols; c++)
        {
            grid_width += col_w[c];
        }
        if(ncols > 1) grid_width += pixel_t(ncols - 1) * col_gap;
    }
    ret_width = grid_width;

    // Pass 1: render each item at its column width (normal mode -> auto width
    // stretches, explicit width respected) to measure content height per row.
    std::vector<pixel_t> row_h(nrows, 0_px);
    std::vector<bool>    row_fixed(nrows, false);

    const auto doc   = src_el()->get_document();
    const auto& fmet = css().get_font_metrics();

    // Row percentages resolve against the grid container's *height*; when that
    // height is indefinite the track behaves as auto (CSS Grid 5.1.1).
    const bool    height_definite = (self_size.height.type == containing_block_context::cbc_value_type_absolute);
    const pixel_t height_base     = height_definite ? self_size.height.value : 0_px;

    // row-gap resolves its percentage against the container's block size
    // (height); an indefinite height makes the percentage gap behave as 0.
    const pixel_t row_gap = css().get_row_gap().calc_percent(height_base);

    for(int r = 0; r < nrows; r++)
    {
        if(r >= static_cast<int>(rows_t.size())) break;
        // A minmax() row sizes toward its max bound; the min bound floors it below.
        const css_length& tr = rows_t[r].is_minmax ? rows_t[r].max : rows_t[r].min;
        if(tr.is_predefined() || tr.units() == css_units_fr) continue; // auto/fr -> content sized
        if(tr.units() == css_units_percentage && !height_definite) continue; // indefinite -> auto
        row_h[r] = doc->to_pixels(tr, fmet, height_base);
        if(rows_t[r].is_minmax)
        {
            const css_length& mn = rows_t[r].min;
            if(!mn.is_predefined() && mn.units() != css_units_fr &&
               (mn.units() != css_units_percentage || height_definite))
            {
                pixel_t mn_px = doc->to_pixels(mn, fmet, height_base);
                if(row_h[r] < mn_px) row_h[r] = mn_px;
            }
        }
        row_fixed[r] = true;
    }

    // Width/height spanned by an item across its tracks, including the gaps
    // between the spanned tracks.
    auto span_w = [&](int col_start, int span) -> pixel_t {
        pixel_t w = 0_px;
        for(int k = 0; k < span; k++)
        {
            w += col_w[col_start + k];
            if(k) w += col_gap;
        }
        return w;
    };
    auto span_h = [&](int row_start, int span) -> pixel_t {
        pixel_t h = 0_px;
        for(int k = 0; k < span; k++)
        {
            h += row_h[row_start + k];
            if(k) h += row_gap;
        }
        return h;
    };

    // Precompute effective self alignment per item (justify-self = inline axis,
    // align-self = block axis), resolving auto/normal against the container.
    const int nitems = static_cast<int>(items.size());
    std::vector<int> eff_js(nitems), eff_as(nitems);
    for(int i = 0; i < nitems; i++)
    {
        eff_js[i] = effective_self_align(items[i], true);
        eff_as[i] = effective_self_align(items[i], false);
    }

    // The inline content-box size an item is laid out at: the full span when
    // justify-self is stretch, otherwise the item shrink-wraps (explicit width
    // respected; auto width becomes the max-content width). natural_width is
    // cached because it is used in both passes.
    m_natw.assign(nitems, -1_px);
    auto inline_size = [&](int i) -> pixel_t {
        const auto& el   = items[i];
        pixel_t     span = span_w(areas[i].col_start, areas[i].col_span);
        if(base_align(eff_js[i]) == flex_align_items_stretch) return span;
        pixel_t w;
        if(el->css().get_width().is_predefined())
        {
            if(m_natw[i] < 0_px) m_natw[i] = natural_item_width(el, self_size, fmt_ctx);
            w = m_natw[i];
        } else
        {
            w = el->css().get_width().calc_percent(span) + el->content_offset_width();
        }
        // An item never shrink-wraps to less than 0; it may overflow its area.
        if(w < 0_px) w = 0_px;
        return w;
    };

    for(int i = 0; i < nitems; i++)
    {
        const int r = areas[i].row_start;
        // A multi-span item's height is attributed to its first row (spanning
        // distribution across rows is a known C19 simplification).
        items[i]->render(0_px, 0_px, self_size.new_width(inline_size(i) - items[i]->content_offset_width()),
                         fmt_ctx);
        if(!row_fixed[r] && items[i]->bottom() > row_h[r]) row_h[r] = items[i]->bottom();
    }

    // align-content distributes the free block space among/around the rows.
    // The container's block size is definite only when its height is definite;
    // otherwise rows are packed at the content height and there is no free space.
    std::vector<char> row_is_auto(nrows, 1);
    for(int r = 0; r < nrows; r++) row_is_auto[r] = row_fixed[r] ? 0 : 1;
    pixel_t container_h     = height_definite ? height_base : 0_px;
    pixel_t ac_off = 0_px, ac_extra = 0_px;
    if(height_definite)
    {
        int ac = base_align(static_cast<int>(css().get_flex_align_content()));
        distribute_tracks(static_cast<flex_justify_content>(ac), container_h, row_gap, row_h, row_is_auto, ac_off,
                          ac_extra);
    }

    // Pass 2: place items into their grid areas. Track offset arrays carry the
    // content-distribution offset and any extra inter-track spacing, so
    // col_x[c]/row_y[r] is the content origin of that track.
    std::vector<pixel_t> col_x(ncols + 1, 0_px);
    col_x[0] = jc_off;
    for(int c = 0; c < ncols; c++)
    {
        col_x[c + 1] = col_x[c] + col_w[c] + col_gap + jc_extra;
    }
    std::vector<pixel_t> row_y(nrows + 1, 0_px);
    row_y[0] = ac_off;
    for(int r = 0; r < nrows; r++)
    {
        row_y[r + 1] = row_y[r] + row_h[r] + row_gap + ac_extra;
    }

    // Baseline alignment (align-self: [first] baseline). For each row, the items
    // that opt into baseline alignment establish a shared first-baseline: the row
    // baseline sits at max(baseline) from the track top, and the track grows to
    // fit the deepest descent below it. Non-baseline items are unaffected.
    std::vector<pixel_t> row_baseline(nrows, -1_px);
    for(int i = 0; i < nitems; i++)
    {
        if(base_align(eff_as[i]) != flex_align_items_baseline) continue;
        const int r = areas[i].row_start;
        pixel_t   b = items[i]->get_first_baseline();
        if(b < 0_px) b = 0_px;
        if(row_baseline[r] < 0_px || b > row_baseline[r]) row_baseline[r] = b;
    }
    for(int i = 0; i < nitems; i++)
    {
        if(base_align(eff_as[i]) != flex_align_items_baseline) continue;
        const int r = areas[i].row_start;
        if(row_baseline[r] < 0_px) continue;
        pixel_t   b   = items[i]->get_first_baseline();
        if(b < 0_px) b = 0_px;
        // Space the item needs below the shared baseline = its descent.
        pixel_t   need = row_baseline[r] + (items[i]->height() - b);
        if(need > row_h[r])
        {
            pixel_t grow = need - row_h[r];
            row_h[r] = need;
            // Shift every later row down by the grown amount.
            for(int rr = r + 1; rr <= nrows; rr++) row_y[rr] += grow;
        }
    }

    for(int i = 0; i < nitems; i++)
    {
        const int c        = areas[i].col_start;
        const int r        = areas[i].row_start;
        auto&     el       = items[i];
        bool      h_auto   = el->css().get_height().is_predefined();
        bool      replaced = el->src_el()->is_replaced();
        const int js       = base_align(eff_js[i]);
        const int as_      = base_align(eff_as[i]);

        // Block axis (align-self): stretch an auto-height, non-replaced item to
        // its area height; otherwise keep its natural height and offset it.
        // `area_w` is the area's inline size (the alignment reference); `iw` is
        // the item's own layout width (the span when stretch, else shrink-wrapped).
        const pixel_t area_w = span_w(c, areas[i].col_span);
        const pixel_t iw     = inline_size(i);
        const pixel_t ih     = span_h(r, areas[i].row_span);
        if(h_auto && !replaced && as_ == flex_align_items_stretch)
        {
            auto cb = self_size.new_width_height(iw - el->content_offset_width(),
                                                 ih - el->content_offset_height(),
                                                 containing_block_context::size_mode_exact_width |
                                                     containing_block_context::size_mode_exact_height);
            el->render(0_px, 0_px, cb, fmt_ctx);
            el->pos().height = ih - el->content_offset_height();
        } else if(js != flex_align_items_stretch)
        {
            // Non-stretch justify-self: re-lay out at the shrink-wrapped width.
            el->render(0_px, 0_px, self_size.new_width(iw - el->content_offset_width()), fmt_ctx);
        }

        const pixel_t item_w = el->width();
        const pixel_t item_h = el->height();

        // Inline axis (justify-self) offset within the area.
        pixel_t dx = 0_px;
        if(js == flex_align_items_center) dx = (area_w - item_w) / 2_px;
        else if(js == flex_align_items_end || js == flex_align_items_self_end || js == flex_align_items_flex_end)
            dx = area_w - item_w;
        // start / self-start / flex-start / stretch -> dx = 0 (stretch already
        // sized the item to the span).

        // Block axis (align-self) offset within the area. Baseline aligns the
        // item's first baseline to the row's shared baseline.
        pixel_t dy = 0_px;
        if(as_ == flex_align_items_center) dy = (ih - item_h) / 2_px;
        else if(as_ == flex_align_items_end || as_ == flex_align_items_self_end || as_ == flex_align_items_flex_end)
            dy = ih - item_h;
        else if(as_ == flex_align_items_baseline && r < static_cast<int>(row_baseline.size()) &&
                row_baseline[r] >= 0_px)
        {
            pixel_t b = el->get_first_baseline();
            if(b < 0_px) b = 0_px;
            dy = row_baseline[r] - b;
            if(dy < 0_px) dy = 0_px;
        }

        el->pos().x = col_x[c] + dx + el->content_offset_left();
        el->pos().y = row_y[r] + dy + el->content_offset_top();
    }

    m_pos.width = grid_width;
    // row_y[nrows] - row_y[0] is the tracks' total block size plus every inter-row
    // gap and align-content extra spacing; the container's content height drops
    // the single trailing gap after the last row (and the content-distribution
    // offset at row_y[0] is not part of the content size).
    pixel_t content_h = row_y[nrows] - row_y[0] - (nrows > 0 ? (row_gap + ac_extra) : 0_px);
    if(height_definite && content_h < height_base) content_h = height_base;
    m_pos.height = content_h;

    m_pos.move_to(x, y);
    m_pos.x += content_offset_left();
    m_pos.y += content_offset_top();

    return {ret_width, ret_width};
}

std::shared_ptr<litehtml::render_item> litehtml::render_item_grid::init()
{
    decltype(m_children) new_children;
    decltype(m_children) inlines;

    // Grid items are blockified; inline-level runs are wrapped in anonymous
    // block boxes (same approach as render_item_flex).
    auto convert_inlines = [&]() {
        if(!inlines.empty())
        {
            auto not_space =
                std::find_if(inlines.rbegin(), inlines.rend(),
                             [&](const std::shared_ptr<render_item>& el) { return !el->src_el()->is_space(); });
            if(not_space != inlines.rend())
            {
                inlines.erase((not_space.base()), inlines.end());
            }

            auto anon_el = std::make_shared<html_tag>(src_el());
            auto anon_ri = std::make_shared<render_item_block>(anon_el);
            for(const auto& inl : inlines)
            {
                anon_ri->add_child(inl);
            }
            anon_ri->parent(shared_from_this());

            new_children.push_back(anon_ri->init());
            inlines.clear();
        }
    };

    for(const auto& el : m_children)
    {
        if(el->src_el()->css().get_display() == display_inline_text)
        {
            if(!inlines.empty())
            {
                inlines.push_back(el);
            } else
            {
                if(!el->src_el()->is_white_space())
                {
                    inlines.push_back(el);
                }
            }
        } else
        {
            convert_inlines();
            if(el->src_el()->is_block_box())
            {
                el->parent(shared_from_this());
                new_children.push_back(el->init());
            } else
            {
                auto anon_el = std::make_shared<html_tag>(el->src_el());
                auto anon_ri = std::make_shared<render_item_block>(anon_el);
                anon_ri->add_child(el->init());
                anon_ri->parent(shared_from_this());
                new_children.push_back(anon_ri->init());
            }
        }
    }
    convert_inlines();
    children() = new_children;

    return shared_from_this();
}
