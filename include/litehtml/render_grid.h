#ifndef LITEHTML_RENDER_GRID_H
#define LITEHTML_RENDER_GRID_H

#include "render_block.h"

namespace litehtml
{
    // CSS Grid layout.
    //
    // Supported:
    //   * display: grid / inline-grid establishes a grid container.
    //   * grid-template-columns/rows with px (and other absolute units), %, fr,
    //     auto/min-content/max-content, repeat(<n>,...) and minmax(min,max).
    //   * gap / row-gap / column-gap.
    //   * Explicit line-based item placement (grid-column/row start/end, span)
    //     and automatic placement with grid-auto-flow (row/column, sparse/dense),
    //     growing implicit tracks.
    //   * Auto rows sized to the tallest item in the row; items stretch to fill
    //     their grid area (the default align/justify behavior).
    //
    // Not yet supported (honest fallback, see plan C19): named lines/areas,
    // grid-auto-columns/rows track sizing, subgrid, and the align/justify
    // properties. Unresolvable items fall back to content/auto sizing rather
    // than mocked geometry.
    class render_item_grid : public render_item_block
    {
        // A placed grid item's cell range (0-based track indices).
        struct grid_item_area
        {
            int col_start = 0;
            int col_span  = 1;
            int row_start = 0;
            int row_span  = 1;
        };

        rendered_width _render_content(pixel_t x, pixel_t y, bool second_pass,
                                       const containing_block_context& self_size, formatting_context* fmt_ctx) override;

        // Resolve one placement axis (grid-column or grid-row start/end) into a
        // definite start track (0-based) plus a span. Returns -1 for the start
        // when it is auto (to be auto-placed). `explicit_count` is the number of
        // explicit tracks in that axis (for resolving negative line numbers).
        int resolve_axis(const grid_line& start, const grid_line& end, int explicit_count, int& out_span);

        // Compute every in-flow item's cell range and the resulting track counts
        // (which may grow past the explicit grid via implicit tracks).
        std::vector<grid_item_area> place_items(const std::vector<std::shared_ptr<render_item>>& items,
                                                int explicit_cols, int explicit_rows, int& out_ncols, int& out_nrows);

        // Resolve the explicit column track list into pixel sizes. `available` is
        // the container content-box width (the % base and fr free-space source)
        // when `available_definite`; otherwise (shrink-to-fit) percentage/fr/auto
        // tracks are content sized. Auto/min/max-content tracks are measured from
        // the items placed in that column (per `areas`). `col_gap` is the resolved
        // column-gap; it is subtracted from the free space distributed to fr
        // tracks. A minmax() track sizes toward its max bound, floored at its min.
        std::vector<pixel_t> resolve_columns(const grid_track_vector& tracks, pixel_t available, bool available_definite,
                                             const std::vector<std::shared_ptr<render_item>>& items,
                                             const std::vector<grid_item_area>& areas, int ncols,
                                             const containing_block_context& self_size,
                                             formatting_context* fmt_ctx, pixel_t col_gap);

      public:
        explicit render_item_grid(std::shared_ptr<element> src_el) :
            render_item_block(std::move(src_el))
        {
        }

        std::shared_ptr<render_item> clone() override
        {
            return std::make_shared<render_item_grid>(src_el());
        }
        std::shared_ptr<render_item> init() override;
    };
} // namespace litehtml

#endif // LITEHTML_RENDER_GRID_H
