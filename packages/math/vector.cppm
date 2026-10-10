module;
#include <array>
#include <assert.hpp>
#include <bit>
#include <cmath>
#include <cstddef>
#include <immintrin.h>
#include <limits>
#include <type_traits>
#include <utility>
export module nova.math.vector;

namespace nova::math {
    export template<std::size_t Dim>
    requires(Dim >= 2 && Dim <= 4)
    class alignas(Dim == 2 ? 8 : 16) vector final {
    public:
        using value_type = float;
        using size_type = std::size_t;
        using array_type = std::array<float, Dim>;

        static constexpr size_type dimension = Dim;

        constexpr vector() noexcept
            :
            m_storage(make_storage(0.0f, 0.f, 0.0f, 0.0f))
        {}

        constexpr explicit vector(const float v) noexcept
            :
            m_storage(make_storage(v, v, Dim >= 3 ? v : 0.0f, Dim == 4 ? v : 0.0f))
        {}

        constexpr vector(const float x, const float y) noexcept requires(Dim == 2)
            :
            m_storage(make_storage(x,y,0.0f,0.0f))
        {}

        constexpr vector(const float x, const float y, const float z) noexcept requires(Dim == 3)
            :
            m_storage(make_storage(x,y,z,0.0f))
        {}

        constexpr vector(const float x, const float y, const float z, const float w) noexcept requires(Dim == 4) 
            :
            m_storage(make_storage(x,y,z,w))
        {}

        explicit constexpr vector(const array_type& values) noexcept
            :
            m_storage(storage_from_array(values))
        {}

        vector(const vector&) = default;
        vector& operator=(const vector&) = default;
        vector(vector&&) = default;
        vector& operator=(vector&&) = default;
        ~vector() noexcept = default;

        [[nodiscard]] static constexpr vector zero() noexcept {
            return{};
        }

        [[nodiscard]] static constexpr vector one() noexcept {
            return vector{1.0f};
        }

        [[nodiscard]] static constexpr vector unit_x() noexcept {
            if constexpr (Dim == 2) {
                return {1.0f, 0.0f};
            }
            else if constexpr (Dim == 3) {
                return {1.0f, 0.0f, 0.0f};
            }
            else {
                return {1.0f, 0.0f, 0.0f, 0.0f};
            }
        }

        [[nodiscard]] static constexpr vector unit_y() noexcept {
            if constexpr (Dim == 2) {
                return {0.0f, 1.0f};
            }
            else if constexpr (Dim == 3) {
                return {0.0f, 1.0f, 0.0f};
            }
            else {
                return {0.0f, 1.0f, 0.0f, 0.0f};
            }
        }


        [[nodiscard]] static constexpr vector unit_z() noexcept requires(Dim >= 3) {
            if constexpr (Dim == 3) {
                return {0.0f, 0.0f, 1.0f};
            }
            else {
                return {0.0f, 0.0f, 1.0f, 0.0f};
            }
        }

        [[nodiscard]] static constexpr vector unit_w() noexcept requires(Dim == 4) {
            return {0.0f, 0.0f, 0.0f, 1.0f};
        }

        [[nodiscard]] static constexpr size_type size() noexcept {
            return Dim;
        }

        template<size_type I>
        requires(I < Dim)
        [[nodiscard]] constexpr float get() const noexcept {
            if constexpr (Dim == 2) {
                return m_storage[I];
            }
            else {
                return std::bit_cast<physical_array>(m_storage)[I];
            }
        }

        template<size_type I>
        requires(I < Dim)
        constexpr void set(const float v) noexcept {
            if constexpr (Dim == 2) {
                m_storage[I] = v;
            }
            else {
                auto lanes = std::bit_cast<physical_array>(m_storage);
                lanes[I] = v;

                if constexpr (Dim == 3) {
                    lanes[3] = 0.0f;
                }
                m_storage = std::bit_cast<__m128>(lanes);
            }
        }

        [[nodiscard]] constexpr float operator[](const size_type index) const noexcept {
            DEBUG_ASSERT(index < Dim);

            if constexpr (Dim == 2) {
                return m_storage[index];
            }
            else {
                if consteval {
                    return std::bit_cast<physical_array>(m_storage)[index];
                }
                else {
                    const __m128i lane = _mm_cvtsi32_si128(static_cast<int>(index));
                    return _mm_cvtss_f32(_mm_permutevar_ps(load_simd(), lane));
                }
            }
        }

        constexpr void set(const size_type index, const float value) noexcept {
            DEBUG_ASSERT(index < Dim);

            if constexpr(Dim == 2) {
                m_storage[index] = value;
                return;
            }
            else {
                switch (index) {
                    case 0:
                        set<0>(value);
                        break;
                    case 1:
                        set<1>(value);
                        break;
                    case 2:
                        set<2>(value);
                        break;
                    case 3:
                        if constexpr (Dim == 4) {
                            set<3>(value);
                        }
                        break;
                    default:
                        std::unreachable();
                }
            }
        }

        [[nodiscard]] constexpr float x() const noexcept {
            return get<0>();
        }

        [[nodiscard]] constexpr float y() const noexcept {
            return get<1>();
        }

        [[nodiscard]] constexpr float z() const noexcept requires(Dim >= 3) {
            return get<2>();
        }

        [[nodiscard]] constexpr float w() const noexcept requires(Dim == 4) {
            return get<3>();
        }

        constexpr void set_x(const float value) noexcept {
            set<0>(value);
        }

        constexpr void set_y(const float value) noexcept {
            set<1>(value);
        }

        constexpr void set_z(const float value) noexcept requires(Dim >= 3) {
            set<2>(value);
        }

        constexpr void set_w(const float value) noexcept requires(Dim == 4) {
            set<3>(value);
        }

        [[nodiscard]] constexpr array_type to_array() const noexcept {
            if constexpr (Dim == 2) {
                return m_storage;
            }
            else {
                const physical_array lanes = std::bit_cast<physical_array>(m_storage);

                if constexpr (Dim == 3) {
                    return {lanes[0], lanes[1], lanes[2]};
                }
                else {
                    return lanes;
                }
            }
        }

        void store(float* const dst) const noexcept {
            DEBUG_ASSERT(dst != nullptr);

            if constexpr (Dim == 2) {
                _mm_storeu_si64(dst, _mm_castps_si128(load_simd()));
            }
            else if constexpr (Dim == 3) {
                const __m128 value = load_simd();
                _mm_storeu_si64(dst, _mm_castps_si128(value));
                _mm_store_ss(dst + 2, _mm_shuffle_ps(value, value, _MM_SHUFFLE(2,2,2,2)));
            }
            else {
                _mm_storeu_ps(dst, load_simd());
            }
        }

        [[nodiscard]] constexpr vector operator+() const noexcept {
            return *this;
        }

        [[nodiscard]] constexpr vector operator-() const noexcept {
            if consteval {
                array_type values = to_array();
                for(float& v : values) {
                    v = -v;
                }
                return vector{values};
            }
            else {
                return from_simd(_mm_xor_ps(load_simd(), active_sign_mask()));
            }
        }

        [[nodiscard]] constexpr vector operator+(const vector& rhs) const noexcept {
            if consteval {
                return apply(rhs, [](const float lhs, const float r) constexpr noexcept {
                    return lhs + r;
                });
            }
            else {
                return from_simd(_mm_add_ps(load_simd(), rhs.load_simd()));
            }
        }

        [[nodiscard]] constexpr vector operator-(const vector& rhs) const noexcept {
            if consteval {
                 return apply(rhs, [](const float lhs, const float r) constexpr noexcept {
                    return lhs - r;
                });
            }
            else {
                return from_simd(_mm_sub_ps(load_simd(), rhs.load_simd()));
            }
        }

        [[nodiscard]] constexpr vector operator/(const vector& rhs) const noexcept {
            if consteval {
                 return apply(rhs, [](const float lhs, const float r) constexpr noexcept {
                    return lhs / r;
                });
            }
            else {
                return from_simd(_mm_div_ps(load_simd(), division_denominator(rhs.load_simd())));
            }
        }

        [[nodiscard]] constexpr vector operator*(const vector& rhs) const noexcept {
            if consteval {
                 return apply(rhs, [](const float lhs, const float r) constexpr noexcept {
                    return lhs * r;
                });
            }
            else {
                return from_simd(_mm_mul_ps(load_simd(), rhs.load_simd()));
            }
        }

        [[nodiscard]] constexpr vector operator+(const float scalar) const noexcept {
            if consteval {
                return apply(scalar, [](const float lhs, const float rhs) constexpr noexcept {
                    return lhs + rhs;
                });
            }
            else {
                return from_simd(_mm_add_ps(load_simd(), active_splat(scalar)));
            }
        }

        [[nodiscard]] constexpr vector operator-(const float scalar) const noexcept {
            if consteval {
                return apply(scalar, [](const float lhs, const float rhs) constexpr noexcept {
                    return lhs - rhs;
                });
            }
            else {
                return from_simd(_mm_sub_ps(load_simd(), active_splat(scalar)));
            }
        }

        [[nodiscard]] constexpr vector operator*(const float scalar) const noexcept {
            if consteval {
                return apply(scalar, [](const float lhs, const float rhs) constexpr noexcept {
                    return lhs * rhs;
                });
            }
            else {
                return from_simd(_mm_mul_ps(load_simd(), active_splat(scalar)));
            }
        }

        [[nodiscard]] constexpr vector operator/(const float scalar) const noexcept {
            if consteval {
                return apply(scalar, [](const float lhs, const float rhs) constexpr noexcept {
                    return lhs / rhs;
                });
            }
            else {
                return from_simd(_mm_div_ps(load_simd(), active_divisor(scalar)));
            }
        }

        constexpr vector& operator+=(const vector& rhs) noexcept {
            *this = *this + rhs;
            return *this;
        }

        constexpr vector& operator-=(const vector& rhs) noexcept {
            *this = *this - rhs;
            return *this;
        }

        constexpr vector& operator*=(const vector& rhs) noexcept {
            *this = *this * rhs;
            return *this;
        }

        constexpr vector& operator/=(const vector& rhs) noexcept {
            *this = *this / rhs;
            return *this;
        }

        constexpr vector& operator+=(const float scalar) noexcept {
            *this = *this + scalar;
            return *this;
        }

        constexpr vector& operator-=(const float scalar) noexcept {
            *this = *this - scalar;
            return *this;
        }

        constexpr vector& operator*=(const float scalar) noexcept {
            *this = *this * scalar;
            return *this;
        }

        constexpr vector& operator/=(const float scalar) noexcept {
            *this = *this / scalar;
            return *this;
        }

        [[nodiscard]] constexpr bool operator==(const vector& rhs) const noexcept {
            if consteval {
                return to_array() == rhs.to_array();
            }
            else {
                const int mask = _mm_movemask_ps(_mm_cmp_ps(load_simd(), rhs.load_simd(), _CMP_EQ_OQ));
                return (mask & active_lane_bits) == active_lane_bits;
            }
        }

        [[nodiscard]] constexpr float length_squared() const noexcept {
            return dot(*this, *this);
        }

        [[nodiscard]] float length() const noexcept {
            const __m128 squared = dot_simd(load_simd(), load_simd());
            return _mm_cvtss_f32(_mm_sqrt_ss(squared));
        }

        [[nodiscard]] vector normalized() const noexcept {
            const __m128 value = load_simd();
            const __m128 squared = dot_simd(value, value);

            [[maybe_unused]] const float scalar_squared = _mm_cvtss_f32(squared);
            DEBUG_ASSERT(scalar_squared > 0.0f && std::isfinite(scalar_squared));

            const __m128 inv_length = _mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(squared));
            return from_simd(_mm_mul_ps(value, broadcast_active(inv_length)));
        }

        [[nodiscard]] vector normalized_fast() const noexcept {
            const __m128 value = load_simd();
            const __m128 squared = dot_simd(value, value);

            [[maybe_unused]] const float scalar_squared = _mm_cvtss_f32(squared);
            DEBUG_ASSERT(scalar_squared > 0.0f && std::isfinite(scalar_squared));

            const __m128 inverse_length = reciprocal_sqrt_refined(squared);
            return from_simd(_mm_mul_ps(value, broadcast_active(inverse_length)));
        }

        [[nodiscard]] vector normalized_or_zero(const float epsilon = 1.0e-12f) const noexcept {
            const __m128 value = load_simd();
            const __m128 squared = dot_simd(value, value);
            const float scalar_squared = _mm_cvtss_f32(squared);

            if(!(scalar_squared > epsilon * epsilon) || !std::isfinite(scalar_squared)) [[unlikely]] {
                return zero();
            }

            const __m128 inverse_length = _mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(squared));
            return from_simd(_mm_mul_ps(value, broadcast_active(inverse_length)));
        }

        vector& normalize() noexcept {
            *this = normalized();
            return *this;
        }

        [[nodiscard]] bool is_finite() const noexcept {
            const __m128 abs = _mm_andnot_ps(_mm_set1_ps(-0.0f), load_simd());
            const __m128 finite = _mm_cmp_ps(abs, _mm_set1_ps(std::numeric_limits<float>::infinity()), _CMP_LT_OQ);
            const int mask = _mm_movemask_ps(finite);
            return (mask & active_lane_bits) == active_lane_bits;
        }

        [[nodiscard]] friend constexpr float dot(const vector& lhs, const vector& rhs) noexcept {
            if consteval {
                const auto lhs_values = lhs.to_array();
                const auto rhs_values = rhs.to_array();
                float res = 0.0f;

                for(size_type i = 0; i < Dim; ++i) {
                    res += lhs_values[i] * rhs_values[i];
                }
                return res;
            }
            else {
                return _mm_cvtss_f32(dot_simd(lhs.load_simd(), rhs.load_simd()));
            }
        }

        [[nodiscard]] friend constexpr vector cross(const vector& lhs, const vector& rhs) noexcept requires(Dim == 3) {
            if consteval {
                return {
                    lhs.y() * rhs.z() - lhs.z() * rhs.y(),
                    lhs.z() * rhs.x() - lhs.x() * rhs.z(),
                    lhs.x() * rhs.y() - lhs.y() * rhs.x()
                };
            }
            else {
                const __m128 a = lhs.load_simd();
                const __m128 b = rhs.load_simd();
                const __m128 a_yzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
                const __m128 b_yzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
                const __m128 zxy = _mm_fmsub_ps(a, b_yzx, _mm_mul_ps(a_yzx, b));
                return from_simd(_mm_shuffle_ps(zxy, zxy, _MM_SHUFFLE(3, 0, 2, 1)));
            }
        }

        [[nodiscard]] friend constexpr float distance_squared(const vector& lhs, const vector& rhs) noexcept {
            return (lhs - rhs).length_squared();
        }

        [[nodiscard]] friend float distance(const vector& lhs, const vector& rhs) noexcept {
            return (lhs - rhs).length();
        }

        [[nodiscard]] friend constexpr vector lerp(const vector& lhs, const vector& rhs, const float t) noexcept {
            if consteval {
                return lhs + (rhs - lhs) * t;
            }
            else {
                const __m128 a = lhs.load_simd();
                const __m128 delta = _mm_sub_ps(rhs.load_simd(), a);
                return from_simd(_mm_fmadd_ps(delta, active_splat(t), a));
            }
        }

        [[nodiscard]] friend vector reflect(const vector& incident, const vector& normal) noexcept {
            const __m128 incident_value = incident.load_simd();
            const __m128 normal_value = normal.load_simd();
            const __m128 dot_value = dot_simd(incident_value, normal_value);
            const __m128 factor = _mm_add_ss(dot_value, dot_value);
            return from_simd(_mm_fnmadd_ps(normal_value, broadcast_active(factor), incident_value));
        }

        [[nodiscard]] friend vector project(const vector& value, const vector& onto) noexcept {
            const __m128 value_simd = value.load_simd();
            const __m128 onto_simd = onto.load_simd();
            const __m128 denominator = dot_simd(onto_simd, onto_simd);

            [[maybe_unused]] const float scalar_denominator = _mm_cvtss_f32(denominator);
            DEBUG_ASSERT(scalar_denominator > 0.0f && std::isfinite(scalar_denominator));

            const __m128 numerator = dot_simd(value_simd, onto_simd);
            const __m128 scale = _mm_div_ss(numerator, denominator);

            return from_simd(_mm_mul_ps(onto_simd, broadcast_active(scale)));        
        }

        [[nodiscard]] friend vector reject(const vector& value, const vector& from) noexcept {
            const __m128 value_v = value.load_simd();
            const __m128 from_v  = from.load_simd();
            const __m128 denominator = dot_simd(from_v, from_v);

            [[maybe_unused]] const float scalar_denominator = _mm_cvtss_f32(denominator);
            DEBUG_ASSERT(scalar_denominator > 0.0F && std::isfinite(scalar_denominator));

            const __m128 numerator = dot_simd(value_v, from_v);
            const __m128 scale     = _mm_div_ss(numerator, denominator);

            return from_simd(_mm_fnmadd_ps(from_v, broadcast_active(scale), value_v));
        }

        [[nodiscard]] friend constexpr vector operator+(const float scalar, const vector& rhs) noexcept {
            return rhs + scalar;
        }

        [[nodiscard]] friend constexpr vector operator*(const float scalar, const vector& rhs) noexcept {
            return rhs * scalar;
        }

        [[nodiscard]] friend constexpr vector operator-(const float scalar, const vector& rhs) noexcept {
            if consteval {
                array_type values = rhs.to_array();
                for(float& value : values) {
                    value = scalar - value;
                }
                return vector{values};
            }
            else {
                return from_simd(_mm_sub_ps(active_splat(scalar), rhs.load_simd()));
            }
        }

        [[nodiscard]] friend constexpr vector operator/(const float scalar, const vector& rhs) noexcept {
            if consteval {
                array_type values = rhs.to_array();
                for(float& value : values) {
                    value = scalar / value;
                }
                return vector{values};
            }
            else {
                return from_simd(_mm_div_ps(active_splat(scalar), division_denominator(rhs.load_simd())));
            }
        }
    private:
        using physical_array = std::array<float, 4>;
        using storage_type = std::conditional_t<Dim == 2, std::array<float, 2>, __m128>;

        struct uninitialized_t final {};
        static constexpr uninitialized_t uninitialized{};
        static constexpr int active_lane_bits = (1 << Dim) - 1;

        explicit vector(uninitialized_t) noexcept {}

        [[nodiscard]] static constexpr storage_type make_storage(float x, float y, float z, float w) noexcept {
            if constexpr (Dim == 2) {
                return {x,y};
            }
            else {
                return std::bit_cast<__m128>(physical_array{x,y,z,w});
            }
        }

        [[nodiscard]] static constexpr storage_type storage_from_array(const array_type& values) noexcept {
            if constexpr (Dim == 2) {
                return values;
            }
            else if constexpr (Dim == 3) {
                return std::bit_cast<__m128>(physical_array{values[0], values[1], values[2], 0.0f});
            }
            else {
                return std::bit_cast<__m128>(values);
            }
        }

        [[nodiscard]] __m128 load_simd() const noexcept {
            if constexpr (Dim == 2) {
                return _mm_castsi128_ps(_mm_loadu_si64(m_storage.data()));
            }
            else {
                if constexpr (Dim == 3) {
                    const auto storage = std::bit_cast<physical_array>(m_storage);
                    DEBUG_ASSERT(storage[3] == 0.0f);
                }
                return m_storage;
            }
        }

        [[nodiscard]] static vector from_simd(const __m128  value) noexcept {
            vector vec{uninitialized};

            if constexpr (Dim == 2) {
                _mm_storeu_si64(vec.m_storage.data(), _mm_castps_si128(value));
            }
            else {
                vec.m_storage = value;
            }

            return vec;
        }

        [[nodiscard]] static __m128 active_sign_mask() noexcept {
            constexpr int sign_bit = std::numeric_limits<int>::min();

            if constexpr (Dim == 2) {
                return _mm_castsi128_ps(_mm_setr_epi32(sign_bit, sign_bit, 0, 0));
            }
            else if constexpr (Dim == 3) {
                return _mm_castsi128_ps(_mm_setr_epi32(sign_bit, sign_bit, sign_bit, 0));
            }
            else {
                return _mm_set1_ps(-0.0f);
            }
        }

        [[nodiscard]] static __m128 active_splat(const float v) noexcept {
            if constexpr(Dim == 2) {
                return _mm_setr_ps(v, v, 0.0F, 0.0f);
            }
            else if constexpr(Dim == 3) {
                return _mm_setr_ps(v, v, v, 0.0f);
            }
            else {
                return _mm_set1_ps(v);
            }
        }

        [[nodiscard]] static __m128 active_divisor(const float v) noexcept {
            if constexpr(Dim == 2) {
                return _mm_setr_ps(v, v, 1.0f, 1.0f);
            }
            else if constexpr(Dim == 3) {
                return _mm_setr_ps(v, v, v, 1.0f);
            }
            else {
                return _mm_set1_ps(v);
            }
        }

        [[nodiscard]] static __m128 division_denominator(const __m128 v) noexcept {
            if constexpr (Dim == 2) {
                return _mm_blend_ps(v, _mm_set1_ps(1.0f), 0b1100);
            }
            else if constexpr (Dim == 3) {
                return _mm_blend_ps(v, _mm_set1_ps(1.0f), 0b1000);
            }
            else {
                return v;
            }
        }

        [[nodiscard]] static __m128 broadcast_active(const __m128 scalar) noexcept {
            const __m128 broadcast = _mm_shuffle_ps(scalar, scalar, _MM_SHUFFLE(0, 0, 0, 0));

            if constexpr(Dim == 2) {
                return _mm_blend_ps(broadcast, _mm_setzero_ps(), 0b1100);
            }
            else if constexpr(Dim == 3) {
                return _mm_blend_ps(broadcast, _mm_setzero_ps(), 0b1000);
            }
            else {
                return broadcast;
            }
        }

        [[nodiscard]] static __m128 dot_simd(const __m128 lhs, const __m128 rhs) noexcept {
            const __m128 prod = _mm_mul_ps(lhs, rhs);

            if constexpr (Dim == 2) {
                const __m128 y = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(1,1,1,1));
                return _mm_add_ss(prod, y);
            }
            else {
                const __m128 high = _mm_movehl_ps(prod, prod);
                const __m128 pair_sum = _mm_add_ps(prod, high);
                const __m128 y_w = _mm_shuffle_ps(pair_sum, pair_sum, _MM_SHUFFLE(1,1,1,1));
                return _mm_add_ss(pair_sum, y_w);
            }
        }

        [[nodiscard]] static __m128 reciprocal_sqrt_refined(const __m128 squared) noexcept {
            __m128 estimate = _mm_rsqrt14_ss(squared, squared);
            const __m128 half_squared = _mm_mul_ss(squared, _mm_set_ss(0.5f));
            const __m128 estimate_squared = _mm_mul_ss(estimate, estimate);
            const __m128 correction = _mm_fnmadd_ss(half_squared, estimate_squared, _mm_set_ss(1.5f));

            estimate = _mm_mul_ss(estimate, correction);
            return estimate;
        }

        template<class Op>
        [[nodiscard]] consteval vector apply(const vector& rhs, Op op) const noexcept {
            array_type lhs_values = to_array();
            const array_type rhs_values = rhs.to_array();

            for(size_type i = 0; i < Dim; ++i) {
                lhs_values[i] = op(lhs_values[i], rhs_values[i]);
            }

            return vector{lhs_values};
        }

        template<class Op>
        [[nodiscard]] consteval vector apply(const float scalar, Op op) const noexcept {
            array_type values = to_array();

            for(float& v : values) {
                v = op(v, scalar);
            }

            return vector{values};
        }


        storage_type m_storage;
    };

    export using vec2 = vector<2>;
    export using vec3 = vector<3>;
    export using vec4 = vector<4>;

    static_assert(sizeof(vec2) == 8);
    static_assert(sizeof(vec3) == 16);
    static_assert(sizeof(vec4) == 16);
    static_assert(alignof(vec2) == 8);
    static_assert(alignof(vec3) == 16);
    static_assert(alignof(vec4) == 16);
    static_assert(std::is_trivially_copyable_v<vec2>);
    static_assert(std::is_trivially_copyable_v<vec3>);
    static_assert(std::is_trivially_copyable_v<vec4>);
    static_assert(std::is_standard_layout_v<vec2>);
    static_assert(std::is_standard_layout_v<vec3>);
    static_assert(std::is_standard_layout_v<vec4>);
}
