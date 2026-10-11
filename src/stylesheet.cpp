#include "html.h"
#include <cstdlib>
#include "stylesheet.h"
#include "css_parser.h"
#include "document.h"
#include "document_container.h"

namespace litehtml
{
    // https://www.w3.org/TR/css-syntax-3/#parse-a-css-stylesheet
    template <class Input> // Input == string or css_token_vector
    void css::parse_css_stylesheet(const Input& input, const std::string& baseurl, const std::shared_ptr<document>& doc,
                                   const media_query_list_list::ptr& media, bool top_level)
    {
        if(doc && media)
        {
            doc->add_media_list(media);
        }

        // To parse a CSS stylesheet, first parse a stylesheet.
        auto rules          = css_parser::parse_stylesheet(input, top_level);
        bool import_allowed = top_level;

        // Interpret all of the resulting top-level qualified rules as style rules, defined below.
        // If any style rule is invalid, or any at-rule is not recognized or is invalid according
        // to its grammar or context, it's a parse error. Discard that rule.
        for(const auto& rule : rules)
        {
            if(rule->type == raw_rule::qualified)
            {
                if(parse_style_rule(rule, baseurl, doc, media))
                {
                    import_allowed = false;
                }
                continue;
            }

            // Otherwise: at-rule
            switch(_id(lowcase(rule->name)))
            {
            case _charset_: // ignored  https://www.w3.org/TR/css-syntax-3/#charset-rule
                break;

            case _import_:
                if(import_allowed)
                {
                    parse_import_rule(rule, baseurl, doc, media);
                } else
                {
                    css_parse_error("incorrect placement of @import rule");
                }
                break;

            // https://www.w3.org/TR/css-conditional-3/#at-media
            // @media <media-query-list> { <stylesheet> }
            case _media_:
                {
                    if(rule->block.type != CURLY_BLOCK)
                    {
                        break;
                    }
                    auto new_media = media;
                    auto mq_list   = parse_media_query_list(rule->prelude, doc);
                    // An empty media query list evaluates to true.
                    // https://drafts.csswg.org/mediaqueries-5/#example-6f06ee45
                    if(!mq_list.empty())
                    {
                        new_media = std::make_shared<media_query_list_list>(media ? *media : media_query_list_list());
                        new_media->add(mq_list);
                    }
                    parse_css_stylesheet(rule->block.value, baseurl, doc, new_media, false);
                    import_allowed = false;
                    break;
                }

            default:
                css_parse_error("unrecognized rule @" + rule->name);
            }
        }
    }

    // https://drafts.csswg.org/css-cascade-5/#at-import
    // `layer` and `supports` are not supported
    // @import [ <url> | <string> ] <media-query-list>?
    void css::parse_import_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                                const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media)
    {
        auto tokens = rule->prelude;
        int  index  = 0;
        skip_whitespace(tokens, index);
        auto        tok = at(tokens, index);
        std::string url;
        auto        parse_string = [](const css_token& tok, std::string& str) {
            if(tok.type != STRING)
            {
                return false;
            }
            str = tok.str();
            return true;
        };
        bool ok = parse_url(tok, url) || parse_string(tok, url);
        if(!ok)
        {
            css_parse_error("invalid @import rule");
            return;
        }
        document_container* container = doc->container();
        std::string         css_text;
        std::string         css_baseurl = baseurl;
        container->import_css(css_text, url, css_baseurl);

        auto new_media = media;
        tokens         = slice(tokens, index + 1);
        auto mq_list   = parse_media_query_list(tokens, doc);
        if(!mq_list.empty())
        {
            new_media = std::make_shared<media_query_list_list>(media ? *media : media_query_list_list());
            new_media->add(mq_list);
        }

        parse_css_stylesheet(css_text, css_baseurl, doc, new_media, true);
    }

    // https://www.w3.org/TR/css-syntax-3/#style-rules
    bool css::parse_style_rule(const raw_rule::ptr& rule, const std::string& baseurl,
                               const std::shared_ptr<document>& doc, const media_query_list_list::ptr& media)
    {
        // The prelude of the qualified rule is parsed as a <selector-list>. If this returns failure, the entire style
        // rule is invalid.
        auto list = parse_selector_list(rule->prelude, strict_mode, doc->mode());
        if(list.empty())
        {
            css_parse_error("invalid selector");
            return false;
        }

        style::ptr style = std::make_shared<litehtml::style>(); // style block
        // The content of the qualified rule's block is parsed as a style block's contents.
        style->add(rule->block.value, baseurl, doc->container());

        for(const auto& sel : list)
        {
            sel->m_style       = style;
            sel->m_media_query = media;
            sel->calc_specificity();
            add_selector(sel);
        }
        return true;
    }

    void css::sort_selectors()
    {
        std::sort(m_selectors.begin(), m_selectors.end(),
                  [](const css_selector::ptr& v1, const css_selector::ptr& v2) { return (*v1) < (*v2); });
        m_index.reset();
    }

    // An id, class or attribute name in a compound is a hard requirement that select() checks with
    // the very same comparison (select_attribute bails out when the attribute is absent), so filing
    // a selector under it cannot hide a match. Most selective key first.
    css::compound_key css::key_of(const css_element_selector& compound)
    {
        string_id key_id    = empty_id;
        string_id key_class = empty_id;
        string_id key_attr  = empty_id;
        for(const auto& attr : compound.m_attrs)
        {
            if(attr.name == empty_id)
            {
                continue;
            }
            if(attr.type == select_id && key_id == empty_id)
            {
                key_id = attr.name;
            } else if(attr.type == select_class && key_class == empty_id)
            {
                key_class = attr.name;
            } else if(attr.type == select_attr && key_attr == empty_id)
            {
                key_attr = attr.name;
            }
        }

        if(key_id != empty_id) return {key_kind::id, key_id};
        if(key_class != empty_id) return {key_kind::cls, key_class};
        if(key_attr != empty_id) return {key_kind::attr, key_attr};
        if(compound.m_tag != star_id) return {key_kind::tag, compound.m_tag};
        return {};
    }

    void css::build_index() const
    {
        auto index = std::unique_ptr<selector_index>(new selector_index());

        std::vector<compound_key> is_keys;
        for(size_t i = 0; i < m_selectors.size(); i++)
        {
            const css_element_selector& right = m_selectors[i]->m_right;

            // :root pins the selector to a single element; :is() lets an otherwise keyless compound
            // borrow the keys of its arguments, which is what rescues modern sheets from the
            // universal bucket.
            bool                              needs_root = false;
            const css_attribute_selector*     is_arg     = nullptr;
            for(const auto& attr : right.m_attrs)
            {
                if(attr.type != select_pseudo_class)
                {
                    continue;
                }
                if(attr.name == _root_)
                {
                    needs_root = true;
                } else if(attr.name == _is_ && is_arg == nullptr)
                {
                    is_arg = &attr;
                }
            }

            uint32_t   idx = static_cast<uint32_t>(i);
            const auto key = key_of(right);

            auto file = [&](const compound_key& k) {
                switch(k.kind)
                {
                case key_kind::id: index->by_id[k.name].push_back(idx); break;
                case key_kind::cls: index->by_class[k.name].push_back(idx); break;
                case key_kind::attr: index->by_attr[k.name].push_back(idx); break;
                case key_kind::tag: index->by_tag[k.name].push_back(idx); break;
                case key_kind::none: index->universal.push_back(idx); break;
                }
            };

            if(needs_root)
            {
                index->root_only.push_back(idx);
                continue;
            }
            if(key.kind != key_kind::none)
            {
                file(key);
                continue;
            }

            // :is(a, b) matches if any argument matches, so the selector can be filed under every
            // argument's key. One keyless argument means any element could match, so the whole
            // thing has to stay universal.
            if(is_arg != nullptr && !is_arg->selector_list.empty())
            {
                is_keys.clear();
                bool all_keyed = true;
                for(const auto& sub : is_arg->selector_list)
                {
                    const auto sub_key = key_of(sub->m_right);
                    if(sub_key.kind == key_kind::none)
                    {
                        all_keyed = false;
                        break;
                    }
                    // :is(.a, .a) would otherwise file the same index twice in one bucket, and a
                    // single-bucket lookup does not dedupe.
                    if(std::find(is_keys.begin(), is_keys.end(), sub_key) == is_keys.end())
                    {
                        is_keys.push_back(sub_key);
                    }
                }
                if(all_keyed)
                {
                    for(const auto& k : is_keys)
                    {
                        file(k);
                    }
                    continue;
                }
            }

            {
                index->universal.push_back(idx);
            }
        }

        m_index = std::move(index);
    }

    void css::collect_candidates(string_id tag, string_id id, const std::vector<string_id>& classes,
                                 const string_map& attrs, bool is_root, std::vector<uint32_t>& out) const
    {
        out.clear();

        // Escape hatch: fall back to considering every selector, so a suspected mismatch between
        // the index and select() can be confirmed without rebuilding.
        static const bool index_enabled = [] {
            const char* v = getenv("UV8_LITE_SELECTOR_INDEX");
            return !(v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N'));
        }();
        if(!index_enabled)
        {
            out.resize(m_selectors.size());
            for(size_t i = 0; i < out.size(); i++)
            {
                out[i] = static_cast<uint32_t>(i);
            }
            return;
        }

        if(!m_index)
        {
            build_index();
        }

        int buckets = 0;
        auto take   = [&](const std::unordered_map<int, std::vector<uint32_t>>& map, string_id key) {
            auto it = map.find(key);
            if(it == map.end())
            {
                return;
            }
            out.insert(out.end(), it->second.begin(), it->second.end());
            buckets++;
        };

        if(id != empty_id)
        {
            take(m_index->by_id, id);
        }
        for(string_id cls : classes)
        {
            take(m_index->by_class, cls);
        }
        // m_attrs keys are already lowercased, which is how attribute selectors store their names.
        if(!m_index->by_attr.empty())
        {
            for(const auto& attr : attrs)
            {
                take(m_index->by_attr, _id(attr.first));
            }
        }
        take(m_index->by_tag, tag);
        if(is_root && !m_index->root_only.empty())
        {
            out.insert(out.end(), m_index->root_only.begin(), m_index->root_only.end());
            buckets++;
        }
        if(!m_index->universal.empty())
        {
            out.insert(out.end(), m_index->universal.begin(), m_index->universal.end());
            buckets++;
        }

        // Each bucket is already ascending; only a merge of two or more can be out of order.
        // The dedupe matters because class="a a" pulls the same bucket twice.
        if(buckets > 1)
        {
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
    }

} // namespace litehtml
