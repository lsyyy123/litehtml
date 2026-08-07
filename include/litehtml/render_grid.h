#ifndef LITEHTML_RENDER_GRID_H
#define LITEHTML_RENDER_GRID_H

#include "render_block.h"

namespace litehtml
{
    // Minimal CSS Grid layout (grid-definition / grid-items MVP slice).
    //
    // Supported:
    //   * display: grid / inline-grid establishes a grid container.
    //   * grid-template-columns/rows with px (and other absolute units), %, fr
    //     and auto/min-content/max-content track sizes.
    //   * Automatic row-major item placement (no explicit grid-column/row yet).
    //   * Auto rows sized to the tallest item in the row; items stretch to fill
    //     their grid area (the default align/justify behavior).
    //
    // Not yet supported (honest fallback, see plan C19): repeat(), minmax(),
    // named lines/areas, explicit line-based placement, grid-auto-*, gaps and
    // the align/justify properties. Items that cannot be resolved fall back to
    // content/auto sizing rather than mocked geometry.
    class render_item_grid : public render_item_block
    {
        rendered_width _render_content(pixel_t x, pixel_t y, bool second_pass,
                                       const containing_block_context& self_size, formatting_context* fmt_ctx) override;

        // Resolve the explicit column track list into pixel sizes. `available` is
        // the container content-box width (the % base and fr free-space source)
        // when `available_definite`; otherwise (shrink-to-fit) percentage/fr/auto
        // tracks are content sized. Auto/min/max-content tracks are measured from
        // the items placed in that column. `col_gap` is the resolved column-gap;
        // it is subtracted from the free space distributed to fr tracks. A
        // minmax() track sizes toward its max bound, floored at its min bound.
        std::vector<pixel_t> resolve_columns(const grid_track_vector& tracks, pixel_t available, bool available_definite,
                                             int item_count, const containing_block_context& self_size,
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
