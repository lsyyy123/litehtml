#ifndef LITEHTML_STYLESHEET_H
#define LITEHTML_STYLESHEET_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "css_selector.h"
#include "css_tokenizer.h"

namespace litehtml
{

    // https://www.w3.org/TR/cssom-1/#css-declarations
    struct raw_declaration
    {
        using vector = std::vector<raw_declaration>;

        // property name
        std::string name;
        // default value is specified here to get rid of gcc warning "missing initializer for member"
        css_token_vector value;

        bool important = false;

        operator bool() const
        {
            return name != "";
        }
    };

    // intermediate half-parsed rule that is used internally by the parser
    class raw_rule
    {
      public:
        using ptr    = std::shared_ptr<raw_rule>;
        using vector = std::vector<ptr>;

        enum rule_type
        {
            qualified,
            at
        };

        raw_rule(rule_type type, std::string name = {}) :
            type(type),
            name(std::move(name))
        {
        }

        rule_type type;
        // An at-rule has a name, a prelude consisting of a list of component values, and an optional block consisting
        // of a simple {} block.
        std::string name;
        // https://www.w3.org/TR/css-syntax-3/#qualified-rule
        // A qualified rule has a prelude consisting of a list of component values, and a block consisting of a simple
        // {} block. Note: Most qualified rules will be style rules, where the prelude is a selector and the block a
        // list of declarations.
        css_token_vector prelude;
        css_token        block;
    };

    class css
    {
        css_selector::vector m_selectors;

        // Key-selector index: every selector is filed under one bucket decided by its rightmost
        // compound (id, else class, else attribute name, else tag, else universal). An element only
        // has to look at the buckets its own id/classes/attributes/tag can reach instead of walking
        // the whole sheet, which is what keeps apply_stylesheet from being O(elements x selectors).
        // Buckets hold indices into m_selectors so candidates can be merged back into sheet order.
        struct selector_index
        {
            std::unordered_map<int, std::vector<uint32_t>> by_id;
            std::unordered_map<int, std::vector<uint32_t>> by_class;
            std::unordered_map<int, std::vector<uint32_t>> by_attr;
            std::unordered_map<int, std::vector<uint32_t>> by_tag;
            // :root in the rightmost compound pins the selector to the one root element, so these
            // are worth keeping out of the universal bucket even though they have no other key.
            std::vector<uint32_t> root_only;
            std::vector<uint32_t> universal;
        };

        // What a compound selector demands of an element, used to pick its bucket.
        enum class key_kind
        {
            none,
            id,
            cls,
            attr,
            tag
        };
        struct compound_key
        {
            key_kind  kind = key_kind::none;
            string_id name = empty_id;
            bool      operator==(const compound_key& o) const { return kind == o.kind && name == o.name; }
        };
        static compound_key key_of(const css_element_selector& compound);
        // Built on demand; not thread safe, matching the rest of litehtml.
        mutable std::unique_ptr<selector_index> m_index;

      public:
        const css_selector::vector& selectors() const
        {
            return m_selectors;
        }

        // Fills out (in m_selectors order) with the indices of every selector whose rightmost
        // compound can match an element with the given tag/id/classes/attributes. A candidate still
        // has to go through select(); this only rules out the impossible ones.
        void collect_candidates(string_id tag, string_id id, const std::vector<string_id>& classes,
                                const string_map& attrs, bool is_root, std::vector<uint32_t>& out) const;

        template <class Input>
        void parse_css_stylesheet(const Input& input, const std::string& baseurl, const std::shared_ptr<document>& doc,
                                  const media_query_list_list::ptr& media = nullptr, bool top_level = true);

        void sort_selectors();

      private:
        void build_index() const;

        bool parse_style_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                              const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void parse_import_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                               const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media);
        void add_selector(const css_selector::ptr& selector);
    };

    inline void css::add_selector(const css_selector::ptr& selector)
    {
        selector->m_order = static_cast<int>(m_selectors.size());
        m_selectors.push_back(selector);
        m_index.reset();
    }

} // namespace litehtml

#endif // LITEHTML_STYLESHEET_H
