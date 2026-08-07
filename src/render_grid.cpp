#include "types.h"
#include "render_grid.h"
#include "html_tag.h"
#include "document.h"

#include <algorithm>
#include <vector>

std::vector<litehtml::pixel_t> litehtml::render_item_grid::resolve_columns(const length_vector& tracks,
                                                                           pixel_t available, bool available_definite,
                                                                           int item_count,
                                                                           const containing_block_context& self_size,
                                                                           formatting_context* fmt_ctx, pixel_t col_gap)
{
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

    const auto doc   = src_el()->get_document();
    const auto& fmet = css().get_font_metrics();

    // Measure the max-content width of the items that land in a given column.
    auto measure_column = [&](int col, int ncols) -> pixel_t {
        const int nrows = (static_cast<int>(items.size()) + ncols - 1) / ncols;
        pixel_t   max_w = 0_px;
        for(int r = 0; r < nrows; r++)
        {
            int idx = r * ncols + col;
            if(idx >= static_cast<int>(items.size())) break;
            const auto& el = items[idx];
            pixel_t     w =
                el->render(0_px, 0_px, self_size.new_width(available, containing_block_context::size_mode_content),
                           fmt_ctx)
                    .natural_width;
            w += el->content_offset_width();
            if(w > max_w) max_w = w;
        }
        return max_w;
    };

    // `none` (empty track list) -> a single auto column holding every item.
    if(tracks.empty())
    {
        if(available_definite) return {available};
        return {measure_column(0, 1)};
    }

    const int ncols = static_cast<int>(tracks.size());

    std::vector<pixel_t> col_w(ncols, 0_px);
    std::vector<bool>    is_auto(ncols, false);
    float                total_fr = 0.0f;

    for(int c = 0; c < ncols; c++)
    {
        const css_length& tr = tracks[c];
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
            if(!is_auto[c] && tracks[c].units() != css_units_fr) used += col_w[c];
        }
        if(ncols > 1) used += pixel_t(ncols - 1) * col_gap;
        pixel_t free_space = available - used;
        if(free_space < 0_px) free_space = 0_px;
        for(int c = 0; c < ncols; c++)
        {
            if(!is_auto[c] && tracks[c].units() == css_units_fr)
            {
                col_w[c] = free_space * (tracks[c].val() / total_fr);
            }
        }
    }

    // Content-size the auto (and indefinite-%/fr) columns from their items.
    for(int c = 0; c < ncols; c++)
    {
        if(is_auto[c]) col_w[c] = measure_column(c, ncols);
    }

    return col_w;
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

    const length_vector& cols_t = css().get_grid_template_columns();
    const length_vector& rows_t = css().get_grid_template_rows();

    // column-gap resolves its percentage against the container's inline size
    // (content-box width). row-gap (below) resolves against the block size.
    const pixel_t col_gap = css().get_column_gap().calc_percent(content_width);

    std::vector<pixel_t> col_w = resolve_columns(cols_t, content_width, width_definite,
                                                 static_cast<int>(items.size()), self_size, fmt_ctx, col_gap);
    const int ncols = static_cast<int>(col_w.size());
    const int nrows = (static_cast<int>(items.size()) + ncols - 1) / ncols;

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
        const css_length& tr = rows_t[r];
        if(tr.is_predefined() || tr.units() == css_units_fr) continue; // auto/fr -> content sized
        if(tr.units() == css_units_percentage && !height_definite) continue; // indefinite -> auto
        row_h[r]     = doc->to_pixels(tr, fmet, height_base);
        row_fixed[r] = true;
    }

    for(int i = 0; i < static_cast<int>(items.size()); i++)
    {
        int r = i / ncols;
        int c = i % ncols;
        items[i]->render(0_px, 0_px, self_size.new_width(col_w[c] - items[i]->content_offset_width()), fmt_ctx);
        if(!row_fixed[r] && items[i]->bottom() > row_h[r]) row_h[r] = items[i]->bottom();
    }

    // Pass 2: place items into their grid areas. Only auto-height, non-replaced
    // items are stretched to the row height (align-self: stretch); items with an
    // explicit height and replaced items keep their own size.
    // Track offset arrays include the gap that precedes each track (no gap
    // before the first one), so col_x[c]/row_y[r] is the content origin of that
    // track and the trailing entry is the total grid content size.
    std::vector<pixel_t> col_x(ncols + 1, 0_px);
    for(int c = 0; c < ncols; c++)
    {
        col_x[c + 1] = col_x[c] + col_w[c] + col_gap;
    }
    std::vector<pixel_t> row_y(nrows + 1, 0_px);
    for(int r = 0; r < nrows; r++)
    {
        row_y[r + 1] = row_y[r] + row_h[r] + row_gap;
    }

    for(int i = 0; i < static_cast<int>(items.size()); i++)
    {
        int   r        = i / ncols;
        int   c        = i % ncols;
        auto& el       = items[i];
        bool  h_auto   = el->css().get_height().is_predefined();
        bool  replaced = el->src_el()->is_replaced();
        if(h_auto && !replaced)
        {
            auto cb = self_size.new_width_height(col_w[c] - el->content_offset_width(),
                                                 row_h[r] - el->content_offset_height(),
                                                 containing_block_context::size_mode_exact_width |
                                                     containing_block_context::size_mode_exact_height);
            el->render(0_px, 0_px, cb, fmt_ctx);
            el->pos().height = row_h[r] - el->content_offset_height();
        }
        el->pos().x = col_x[c] + el->content_offset_left();
        el->pos().y = row_y[r] + el->content_offset_top();
    }

    m_pos.width = grid_width;
    // row_y[nrows] carries a trailing gap after the last row; the container's
    // content height has only (nrows-1) gaps between the rows.
    m_pos.height = row_y[nrows] - (nrows > 0 ? row_gap : 0_px);

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
