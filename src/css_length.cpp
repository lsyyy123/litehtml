#include "html.h"
#include "css_length.h"
#include <cmath>
#include <limits>

namespace litehtml
{
    namespace
    {
        struct affine_value
        {
            std::array<double, css_units_fr + 1> coefficients{};
            bool length = false;
            bool percentage = false;
            bool number() const { return !length && !percentage; }
            bool finite() const
            {
                for(double coefficient : coefficients)
                {
                    if(!std::isfinite(coefficient) ||
                       std::abs(coefficient) > std::numeric_limits<float>::max()) return false;
                }
                return true;
            }
        };

        // Parse component values, not author strings. A length-percentage sum
        // retains one coefficient per unit, so neither font nor percentage
        // context has to be guessed at declaration-parse time.
        class calc_parser
        {
            const css_token_vector& m_tokens;
            size_t m_index = 0;
            unsigned m_depth;

            bool whitespace()
            {
                const size_t start = m_index;
                while(m_index < m_tokens.size() && m_tokens[m_index].type == WHITESPACE) ++m_index;
                return start != m_index;
            }
            bool primary(affine_value& out)
            {
                whitespace();
                if(m_index == m_tokens.size() || m_depth > 64) return false;
                const css_token& token = m_tokens[m_index++];
                if(token.type == ROUND_BLOCK ||
                   (token.type == CV_FUNCTION && lowcase(token.name()) == "calc"))
                {
                    return calc_parser(token.value, m_depth + 1).parse(out);
                }
                if(!is_one_of(token.type, NUMBER, PERCENTAGE, DIMENSION) ||
                   !std::isfinite(token.n.number)) return false;
                css_units unit = css_units_none;
                if(token.type == PERCENTAGE)
                {
                    unit = css_units_percentage;
                    out.percentage = true;
                } else if(token.type == DIMENSION)
                {
                    auto idx = css_values(css_units_strings).value_index(lowcase(token.unit()));
                    // fr is a flex, not a length; angles/time/unknown dimensions
                    // likewise cannot participate in a CSS length calculation.
                    if(!idx.has_value() || idx.value() <= css_units_percentage ||
                       idx.value() >= css_units_fr) return false;
                    unit = static_cast<css_units>(idx.value());
                    out.length = true;
                }
                out.coefficients[unit] = token.n.number;
                return true;
            }
            bool product(affine_value& out)
            {
                if(!primary(out)) return false;
                for(;;)
                {
                    const size_t before_space = m_index;
                    whitespace();
                    if(m_index == m_tokens.size() ||
                       (m_tokens[m_index].ch != '*' && m_tokens[m_index].ch != '/'))
                    {
                        m_index = before_space;
                        return true;
                    }
                    const int op = m_tokens[m_index++].ch;
                    affine_value rhs;
                    if(!primary(rhs)) return false;
                    if(op == '/')
                    {
                        if(!rhs.number() || rhs.coefficients[css_units_none] == 0) return false;
                        const double divisor = rhs.coefficients[css_units_none];
                        for(double& coefficient : out.coefficients) coefficient /= divisor;
                    } else
                    {
                        if(!out.number() && !rhs.number()) return false;
                        if(out.number())
                        {
                            const double factor = out.coefficients[css_units_none];
                            out = rhs;
                            for(double& coefficient : out.coefficients) coefficient *= factor;
                        } else
                        {
                            const double factor = rhs.coefficients[css_units_none];
                            for(double& coefficient : out.coefficients) coefficient *= factor;
                        }
                    }
                    if(!out.finite()) return false;
                }
            }
            bool sum(affine_value& out)
            {
                if(!product(out)) return false;
                for(;;)
                {
                    const bool left_space = whitespace();
                    if(m_index == m_tokens.size() ||
                       (m_tokens[m_index].ch != '+' && m_tokens[m_index].ch != '-')) return true;
                    // CSS calc requires whitespace on both sides of + and -.
                    if(!left_space) return false;
                    const int op = m_tokens[m_index++].ch;
                    if(!whitespace()) return false;
                    affine_value rhs;
                    if(!product(rhs) || out.number() != rhs.number()) return false;
                    const double sign = op == '+' ? 1.0 : -1.0;
                    for(size_t unit = 0; unit < out.coefficients.size(); ++unit)
                        out.coefficients[unit] += sign * rhs.coefficients[unit];
                    out.length = out.length || rhs.length;
                    out.percentage = out.percentage || rhs.percentage;
                    if(!out.finite()) return false;
                }
            }
        public:
            calc_parser(const css_token_vector& tokens, unsigned depth = 0) :
                m_tokens(tokens), m_depth(depth) {}
            bool parse(affine_value& out)
            {
                if(!sum(out)) return false;
                whitespace();
                return m_index == m_tokens.size() && out.finite();
            }
        };
    }


    bool css_length::from_token(const css_token& token, int options, css_values keywords)
    {
        if(token.type == CV_FUNCTION && lowcase(token.name()) == "calc")
        {
            affine_value value;
            if(!calc_parser(token.value).parse(value) ||
               (value.length && !(options & f_length)) ||
               (value.percentage && !(options & f_percentage)) ||
               (value.number() && !(options & (f_number | f_integer)))) return false;
            if(value.number())
            {
                double number = value.coefficients[css_units_none];
                if(options & f_integer) number = std::floor(number + 0.5);
                if((options & f_positive) && number < 0) number = 0;
                set_value(static_cast<float>(number), css_units_none);
                return true;
            }
            auto math = std::make_shared<math_value>();
            math->coefficients = value.coefficients;
            math->has_percentage = value.percentage;
            math->non_negative = (options & f_positive) != 0;
            m_math = std::move(math);
            m_value = static_cast<float>(value.coefficients[css_units_px]);
            // Existing layout classifiers use percentage units for indefinite
            // axes/intrinsic sizing. Mixed values must take that same path.
            m_units = value.percentage ? css_units_percentage : css_units_px;
            m_is_predefined = false;
            return true;
        }
        if((options & f_positive) && is_one_of(token.type, NUMBER, DIMENSION, PERCENTAGE) && token.n.number < 0)
        {
            return false;
        }

        if(token.type == IDENT)
        {
            auto idx = keywords.value_index(lowcase(token.name()));
            if(!idx.has_value())
            {
                return false;
            }
            predef(idx.value());
            return true;
        }
        if(token.type == DIMENSION)
        {
            if(!(options & f_length))
            {
                return false;
            }

            auto idx = css_values(css_units_strings).value_index(lowcase(token.unit()));
            // note: 1none and 1\% are invalid
            if(!idx.has_value() || idx.value() == css_units_none || idx.value() == css_units_percentage)
            {
                return false;
            }

            set_value(token.n.number, static_cast<css_units>(idx.value()));
            return true;
        }
        if(token.type == PERCENTAGE)
        {
            if(!(options & f_percentage))
            {
                return false;
            }

            set_value(token.n.number, css_units_percentage);
            return true;
        }
        if(token.type == NUMBER)
        {
            // if token is a nonzero number and neither f_number nor f_integer is specified in the options
            if(!(options & (f_number | f_integer)) && pixel_t(token.n.number) != 0_px)
            {
                return false;
            }
            // if token is a zero number and neither of f_number, f_integer or f_length are specified in the options
            if(!(options & (f_number | f_integer | f_length)) && pixel_t(token.n.number) == 0_px)
            {
                return false;
            }
            if((options & f_integer) && token.n.number_type != css_number_integer)
            {
                return false;
            }

            set_value(token.n.number, css_units_none);
            return true;
        }
        return false;
    }

    std::string css_length::to_string() const
    {
        if(m_is_predefined)
        {
            return "def(" + std::to_string(m_predef) + ")";
        }
        if(m_math)
        {
            std::string res = "calc(";
            bool first = true;
            for(size_t unit = css_units_percentage; unit < css_units_fr; ++unit)
            {
                double coefficient = m_math->coefficients[unit];
                if(coefficient == 0 && !(unit == css_units_percentage && m_math->has_percentage)) continue;
                if(!first) res += coefficient < 0 ? " - " : " + ";
                res += std::to_string(first ? coefficient : std::abs(coefficient));
                res += css_values(css_units_strings).value_by_index(static_cast<int>(unit));
                first = false;
            }
            if(first) res += "0px";
            return res + ")";
        }
        std::string res  = std::to_string(m_value) + "{";
        res             += css_values(css_units_strings).value_by_index(m_units);
        res             += "}";
        return res;
    }

    css_length css_length::predef_value(int val)
    {
        css_length len;
        len.predef(val);
        return len;
    }

} // namespace litehtml
