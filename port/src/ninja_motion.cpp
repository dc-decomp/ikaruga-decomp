// Ninja's motions, natively, as far as they go. Each function computes what its machine code
// computes, in the same order and with the same roundings. IKARUGA_DIFF tests each against its
// machine code on the game's own calls.
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "decomp.h"
#include "diff.h"

namespace game {
namespace {

uint32_t word(uint32_t a) { return guest::load<uint32_t>(a); }
float real(uint32_t a) { return guest::load<float>(a); }

// The key before the frame `frame` among `count` keys of `stride` bytes, which start with their
// frame. The first key is the one before a frame earlier than all.
template <class Frame>
uint32_t search(uint32_t keys, uint32_t stride, uint32_t count, uint32_t frame) {
    uint32_t low = 0, high = count;
    while (high - low > 1) {
        const uint32_t middle = (low + high) >> 1;
        if (frame >= guest::load<Frame>(keys + stride * middle))
            low = middle;
        else
            high = middle;
    }
    return low;
}

// The keys of a 16-bit frame around the frame `frame`, and how far `frame` is from the first to
// the second. After the last key, the second is the first key.
struct Keys {
    uint32_t from, to;
    float t;
};
Keys keys16(uint32_t keys, uint32_t stride, uint32_t count, float frame) {
    const uint32_t i = search<uint16_t>(keys, stride, count, cpu::ftrc(frame) & 0xFFFFu);
    Keys k{keys + stride * i, i >= count - 1 ? keys : keys + stride * (i + 1), 0};
    const int16_t a = guest::load<int16_t>(k.from), b = guest::load<int16_t>(k.to);
    const int16_t span = static_cast<int16_t>(b - a);
    k.t = frame - static_cast<float>(static_cast<uint16_t>(a));
    if (span > 0)
        k.t /= static_cast<float>(span);
    return k;
}

// The keys of a 32-bit frame around the frame `frame`, as keys16. The frames are unsigned: a
// frame beyond 2^31 searches as its value minus 2^32.
Keys keys32(uint32_t keys, uint32_t stride, uint32_t count, float frame) {
    const float search_frame = frame > 2147483648.0f ? frame + -4294967296.0f : frame;
    const uint32_t i = search<uint32_t>(keys, stride, count, cpu::ftrc(search_frame));
    Keys k{keys + stride * i, i >= count - 1 ? keys : keys + stride * (i + 1), 0};
    const int32_t a = guest::load<int32_t>(k.from), b = guest::load<int32_t>(k.to);
    float start = static_cast<float>(a);
    if (a < 0)
        start += 4294967296.0f;
    k.t = frame - start;
    const int32_t span = static_cast<int32_t>(static_cast<uint32_t>(b) - static_cast<uint32_t>(a));
    if (span > 0)
        k.t /= static_cast<float>(span);
    return k;
}

// Calls the function at `function` with these registers from r4 and fr4 on.
uint32_t call(uint32_t function, std::initializer_list<uint32_t> r, std::initializer_list<float> fr = {}) {
    CallArgs args;
    for (const uint32_t v : r) args.r[args.nr++] = v;
    for (const float v : fr) args.fr[args.nfr++] = v;
    return guest::call(function, args).r0;
}

// The motion's state: 0x8C10B4F4 points to it. It holds the current node's data (0), the offset of
// the node's key counts (4), the node's size (8), the current node's key lists (12) and key counts
// (16), the frame (20) and the node's next channel (28). The functions that interpolate a channel
// follow it.
constexpr uint32_t kMotion = 0x8C10B4F4u;

// The next channel of the current node, through the interpolation `function`, or with `function`
// 0 the motion's function at `interpolate`, into `out`. A channel without keys keeps its value:
// returns 0. The function gets the other registers as they came.
uint32_t channel(uint32_t interpolate, uint32_t out, uint32_t r7, float fr5, float fr6, float fr7,
                 uint32_t function = 0) {
    const uint32_t motion = word(kMotion), index = word(motion + 28);
    const uint32_t keys = word(word(motion + 12) + 4 * index);
    if (!keys) {
        guest::store<uint32_t>(motion + 28, index + 1);
        return 0;
    }
    call(function ? function : word(motion + interpolate), {keys, word(word(motion + 16) + 4 * index), out, r7},
         {real(motion + 20), fr5, fr6, fr7});
    const uint32_t after = word(kMotion);
    guest::store<uint32_t>(after + 28, word(after + 28) + 1);
    return 1;
}

// Calls the motion's function at `offset` with `out`, which gets one channel of the node. Returns
// whether the channel has keys.
uint32_t get(uint32_t offset, uint32_t out) { return call(word(word(kMotion) + offset), {out}); }

void copy(uint32_t to, uint32_t from, uint32_t words) {
    for (uint32_t i = 0; i < 4 * words; i += 4) guest::store<uint32_t>(to + i, word(from + i));
}

// A chunk object: its flags (0), model (4), position (8), angles (20), scale (32), child (44),
// sibling (48), and the real part of its rotation as a quaternion (52), whose imaginary part is in
// the angles' place. Flag 8 hides the model, 32 turns the angles in the other order, 64 makes the
// motion skip the object, and 0x400 rotates it by the quaternion.
constexpr uint32_t kPosition = 8, kAngles = 20, kScale = 32, kChild = 44, kSibling = 48, kReal = 52;

// Ninja's motion event flag, and the event's value.
constexpr uint32_t kEventFlag = 0x8C0CD0A4u, kEvent = 0x8C0CD0A8u;

}  // namespace

void njSetNextMotionNodeEx() {
    diff::Check check("njSetNextMotionNodeEx", 0x8C01630Cu);
    const uint32_t motion = word(kMotion), node = word(motion) + word(motion + 8);
    guest::store<uint32_t>(motion, node);
    guest::store<uint32_t>(motion + 12, node);
    guest::store<uint32_t>(motion + 16, node + word(motion + 4));
    guest::store<uint32_t>(motion + 28, 0);
}

// The keys around the frame `arg_fr4` into `param_4` and `param_5`, and the place between them
// into `param_6`.
void _njGetMotionLinearKeys16(gptr<short> param_1, int param_2, uint param_3, gptr<undefined4> param_4,
                              gptr<undefined4> param_5, gptr<float> param_6, float arg_fr4, float arg_fr5) {
    diff::Check check("_njGetMotionLinearKeys16", 0x8C016518u, param_1, param_2, param_3, param_4, param_5, param_6,
                      arg_fr4, arg_fr5);
    const Keys k = keys16(param_1.a, static_cast<uint32_t>(param_2), param_3, arg_fr4);
    guest::store<uint32_t>(param_4.a, k.from);
    guest::store<uint32_t>(param_5.a, k.to);
    guest::store<float>(param_6.a, k.t);
}

// The keys around the frame `arg_fr4` into `param_4` and `param_5`, and the place between them
// into `param_6`, for keys with a 32-bit frame.
void _njGetMotionLinearKeys(gptr<int> param_1, int param_2, uint param_3, gptr<undefined4> param_4, gptr<int> param_5,
                            gptr<float> param_6, float arg_fr4, float arg_fr5) {
    diff::Check check("_njGetMotionLinearKeys", 0x8C016404u, param_1, param_2, param_3, param_4, param_5, param_6,
                      arg_fr4, arg_fr5);
    const Keys k = keys32(param_1.a, static_cast<uint32_t>(param_2), param_3, arg_fr4);
    guest::store<uint32_t>(param_4.a, k.from);
    guest::store<uint32_t>(param_5.a, k.to);
    guest::store<float>(param_6.a, k.t);
}

undefined4 _njGetMotionFloat3(undefined4 param_1, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7,
                              float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("_njGetMotionFloat3", 0x8C0165C8u, param_1, arg_r5, arg_r6, arg_r7, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7);
    return check.returns(channel(0x24, param_1, arg_r7, arg_fr5, arg_fr6, arg_fr7));
}

undefined4 _njGetMotionSangle3(undefined4 param_1, undefined4 arg_r5, undefined4 arg_r6, undefined4 arg_r7,
                               float arg_fr4, float arg_fr5, float arg_fr6, float arg_fr7) {
    diff::Check check("_njGetMotionSangle3", 0x8C01674Eu, param_1, arg_r5, arg_r6, arg_r7, arg_fr4, arg_fr5,
                      arg_fr6, arg_fr7);
    return check.returns(channel(0x30, param_1, arg_r7, arg_fr5, arg_fr6, arg_fr7));
}

// A 16-bit angle channel, always through njMotionIntLinearSA1.
undefined4 _njGetMotionSangle1(gptr<undefined2> param_1, float arg_fr4, float arg_fr5) {
    diff::Check check("_njGetMotionSangle1", 0x8C0166B2u, param_1, arg_fr4, arg_fr5);
    return check.returns(channel(0, param_1.a, 0, arg_fr5, 0.0f, 0.0f, 0x8C016AC4u));
}

namespace {

// `angles` 16-bit angles at `out`, linear between keys of a 16-bit frame and the angles.
void linear_angles(uint32_t keys, uint32_t count, uint32_t out, float frame, uint32_t angles) {
    const Keys k = keys16(keys, 2 + 2 * angles, count, frame);
    for (uint32_t i = 2; i <= 2 * angles; i += 2) {
        const int16_t a = guest::load<int16_t>(k.from + i), b = guest::load<int16_t>(k.to + i);
        const float v = std::fma(k.t, static_cast<float>(static_cast<int16_t>(b - a)), static_cast<float>(a));
        guest::store<uint16_t>(out + i - 2, static_cast<uint16_t>(cpu::ftrc(v)));
    }
}

}  // namespace

// Three 16-bit angles, linear between keys of 8 bytes: the frame, then the angles.
void njMotionIntLinearSA3(gptr<short> param_1, uint param_2, gptr<undefined2> param_3, float arg_fr4,
                          float arg_fr5) {
    diff::Check check("njMotionIntLinearSA3", 0x8C016A2Eu, param_1, param_2, param_3, arg_fr4, arg_fr5);
    linear_angles(param_1.a, param_2, param_3.a, arg_fr4, 3);
}

// One 16-bit angle, linear between keys of 4 bytes.
void njMotionIntLinearSA1(gptr<short> param_1, uint param_2, gptr<undefined2> param_3, float arg_fr4,
                          float arg_fr5) {
    diff::Check check("njMotionIntLinearSA1", 0x8C016AC4u, param_1, param_2, param_3, arg_fr4, arg_fr5);
    linear_angles(param_1.a, param_2, param_3.a, arg_fr4, 1);
}

// Three floats, linear between keys of 16 bytes: a 32-bit frame, then the values.
void njMotionIntLinearF3(gptr<void> keys, uint count, gptr<float> out, float frame) {
    diff::Check check("njMotionIntLinearF3", 0x8C016886u, keys, count, out, frame);
    const Keys k = keys32(keys.a, 16, count, frame);
    for (uint32_t c = 4; c <= 12; c += 4) {
        const float from = real(k.from + c);
        guest::store<float>(out.a + c - 4, std::fma(k.t, real(k.to + c) - from, from));
    }
}

// A point on an Overhauser curve through keys of 16 bytes: the frame, then x, y and z. The curve
// from one key to the next also uses the keys before and after them, and the keys repeat in a loop.
void njMotionIntOverhauserF3(int param_1, uint param_2, gptr<undefined4> param_3, float arg_fr4, float arg_fr5) {
    diff::Check check("njMotionIntOverhauserF3", 0x8C016B64u, param_1, param_2, param_3, arg_fr4, arg_fr5);
    const uint32_t keys = static_cast<uint32_t>(param_1), count = param_2;
    float search_frame = arg_fr4;
    if (!(2147483648.0f > search_frame))
        search_frame -= 4294967296.0f;
    const uint32_t i = search<uint32_t>(keys, 16, count, cpu::ftrc(search_frame));
    const uint32_t before = keys + 16 * _modlu(count, count + i - 1), from = keys + 16 * i;
    const uint32_t to = keys + 16 * _modlu(count, i + 1), after = keys + 16 * _modlu(count, i + 2);

    // The frames are unsigned.
    float span = static_cast<float>(static_cast<int32_t>(word(to) - word(from)));
    if (0.0f > span)
        span += 4294967296.0f;
    float start = static_cast<float>(static_cast<int32_t>(word(from)));
    if (static_cast<int32_t>(word(from)) < 0)
        start += 4294967296.0f;
    // The constant at 0x8C0CA678 (1) divides when the next key is not later.
    const float divisor = static_cast<int32_t>(word(to) - word(from)) > 0 ? span : real(0x8C0CA678u);
    const float t = (arg_fr4 - start) / divisor;

    const float t2 = t * t, t3 = t * t2, two_t2 = t2 * 2.0f;
    const float w_before = ((two_t2 - t) - t3) * 0.5f;
    const float w_from = (t3 * 1.5f - t2 * 2.5f) + 1.0f;
    const float w_to = (t * 0.5f - t3 * 1.5f) + two_t2;
    const float w_after = (t3 - t2) * 0.5f;
    for (uint32_t c = 4; c <= 12; c += 4) {
        float v = w_from * real(from + c);
        v = std::fma(w_before, real(before + c), v);
        v = std::fma(w_to, real(to + c), v);
        v = std::fma(w_after, real(after + c), v);
        guest::store<float>(param_3.a + c - 4, v);
    }
}

// The current motion node's position, rotation and scale for `cnkobj`. A channel without keys keeps
// the object's value. Returns how to rotate: 0 and 1 by angles in either order, 2 by quaternion.
int njGetMotionNodeData(gptr<NJS_CNK_OBJECT> cnkobj, gptr<Float> pos, gptr<Angle> ang, gptr<Float> scl,
                        gptr<NJS_QUATERNION> qua) {
    diff::Check check("njGetMotionNodeData", 0x8C0171BEu, cnkobj, pos, ang, scl, qua);
    const uint32_t object = cnkobj.a;
    const auto order = [&] { return word(object) & 32 ? 1 : 0; };
    const auto object_rotation = [&] {
        if (word(object) & 0x400) {
            guest::store<uint32_t>(qua.a, word(object + kReal));
            copy(qua.a + 4, object + kAngles, 3);
            return 2;
        }
        copy(ang.a, object + kAngles, 3);
        return order();
    };
    if (word(object) & 64) {
        copy(pos.a, object + kPosition, 3);
        copy(scl.a, object + kScale, 3);
        return check.returns(object_rotation());
    }
    if (!get(52, pos.a))
        copy(pos.a, object + kPosition, 3);
    int rotation;
    if (word(word(kMotion) + 56)) {
        rotation = get(56, ang.a) ? order() : object_rotation();
    }
    else if (word(word(kMotion) + 68)) {
        // 16-bit angles.
        guest::Frame frame(8);
        const uint32_t angles = frame.top - 8;
        if (get(68, angles)) {
            for (uint32_t i = 0; i < 3; ++i)
                guest::store<int32_t>(ang.a + 4 * i, guest::load<int16_t>(angles + 2 * i));
            rotation = order();
        }
        else {
            rotation = object_rotation();
        }
    }
    else {
        rotation = get(64, qua.a) ? 2 : object_rotation();
    }
    if (!get(60, scl.a))
        copy(scl.a, object + kScale, 3);
    if (word(kEventFlag) && !_njGetMotionEvent(gptr<undefined2>(kEvent), 0, 0)) {
        const uint16_t event = guest::load<uint16_t>(kEvent);
        guest::store<uint16_t>(kEvent, word(object) & 8 ? event | 1 : event & 0xFFFE);
    }
    njSetNextMotionNodeEx();
    return check.returns(rotation);
}

// Applies the current motion node to the current matrix.
void njMotionTransformEx(gptr<NJS_CNK_OBJECT> cnkobj) {
    diff::Check check("njMotionTransformEx", 0x8C017160u, cnkobj);
    guest::Frame frame(52);
    const uint32_t qua = frame.top - 52, ang = frame.top - 36, scl = frame.top - 24, pos = frame.top - 12;
    const int rotation = njGetMotionNodeData(cnkobj, gptr<Float>(pos), gptr<Angle>(ang), gptr<Float>(scl),
                                             gptr<NJS_QUATERNION>(qua));
    njTranslateEx(gptr<NJS_VECTOR>(pos));
    if (rotation == 0 || rotation == 1)
        njRotateEx(gptr<Angle>(ang), rotation);
    else
        njQuaternionEx(gptr<NJS_QUATERNION>(qua));
    njScaleEx(gptr<NJS_VECTOR>(scl));
}

// Draws `param_1`, its children and its siblings in the motion, each in its own matrix. With the
// event flag set, njChangeMotionObject may change an object for its drawing, and the object gets
// its 56 bytes back after it. The motion callback at 0x8C0CD078 sees each object.
void _njPushPopMotionLink(gptr<NJS_CNK_OBJECT> param_1) {
    diff::Check check("_njPushPopMotionLink", 0x8C017436u, param_1);
    guest::Frame frame(56);
    const uint32_t saved = frame.top - 56;
    uint32_t object = param_1.a;
    do {
        njPushMatrixEx();
        njMotionTransformEx(gptr<NJS_CNK_OBJECT>(object));
        if (word(kEventFlag)) {
            _quick_evn_mvn(56, gptr<void>(saved), gptr<void>(object));
            njChangeMotionObject(gptr<NJS_CNK_OBJECT>(object));
        }
        if (const uint32_t callback = word(0x8C0CD078u))
            call(callback, {object});
        // The draw function of the current model kind, in the table that 0x8C0CD0A0 points to.
        if (word(object + 4) && !(word(object) & 8))
            call(word(word(0x8C0CD0A0u) + 12), {word(object + 4)});
        if (word(kEventFlag))
            _quick_evn_mvn(56, gptr<void>(object), gptr<void>(saved));
        if (word(object + kChild))
            _njPushPopMotionLink(gptr<NJS_CNK_OBJECT>(word(object + kChild)));
        njPopMatrixEx();
        object = word(object + kSibling);
    } while (object);
}

// Makes the motion state at `Slot` of the array at 0x8C10B464 the current one.
void njSetCurMotionInfoSlot(int Slot) {
    diff::Check check("njSetCurMotionInfoSlot", 0x8C0163BCu, Slot);
    guest::store<uint32_t>(kMotion, 0x8C10B464u + 72u * static_cast<uint32_t>(Slot));
}

// Starts `motion` (or the motion at 0x8C0CA66C) at `frame` in the current state. Bits 6 and 7 of
// its interpolation word choose the interpolating functions from the tables at 0x8C0CD080, and
// the rest is its channel count.
void njStartMotionEx(gptr<NJS_MOTION> motion, Float frame) {
    diff::Check check("njStartMotionEx", 0x8C016330u, motion, frame);
    const uint32_t m = motion.a ? motion.a : 0x8C0CA66Cu, state = word(kMotion);
    const uint32_t interpolation = guest::load<uint16_t>(m + 10), table = interpolation & 0xC0 ? 4 : 0;
    const uint32_t counts = 4 * (interpolation & 0xFFFFFF3Fu);
    guest::store<uint32_t>(state, word(m));
    guest::store<uint32_t>(state + 24, word(m + 4));
    guest::store<uint32_t>(state + 32, guest::load<uint16_t>(m + 8));
    guest::store<float>(state + 20, frame);
    guest::store<uint32_t>(state + 4, counts);
    guest::store<uint32_t>(state + 8, 2 * counts);
    guest::store<uint32_t>(state + 12, word(state));
    guest::store<uint32_t>(state + 16, word(state) + counts);
    guest::store<uint32_t>(state + 28, 0);
    guest::store<uint32_t>(state + 36, word(0x8C0CD080u + table));
    guest::store<uint32_t>(state + 40, word(0x8C0CD088u + table));
    guest::store<uint32_t>(state + 44, word(0x8C0CD090u));
    guest::store<uint32_t>(state + 48, word(0x8C0CD098u + table));
}

// Starts `motion` in state 0 and sets the functions that get a node's channels from its type: the
// position (1), the angles (2), 16-bit angles (32) or a quaternion (0x2000), and the scale (4).
// The functions at 0x8C0173DC and 0x8C0173E0 return 0, so the object keeps its values.
void njStartMotionObj(gptr<NJS_MOTION> motion, Float frame) {
    diff::Check check("njStartMotionObj", 0x8C017060u, motion, frame);
    njSetCurMotionInfoSlot(0);
    njStartMotionEx(motion, frame);
    constexpr uint32_t kNone = 0x8C0173DCu, kFloat3 = 0x8C0165C8u;
    const auto set = [](uint32_t offset, uint32_t value) { guest::store<uint32_t>(word(kMotion) + offset, value); };
    const uint32_t type = word(word(kMotion) + 32);
    set(52, type & 1 ? kFloat3 : kNone);
    uint32_t angles = 0x8C0173E0u, quaternion = 0, short_angles = 0;
    if (type & 2) {
        angles = 0x8C016700u;
    }
    else if (type & 32) {
        angles = 0;
        short_angles = 0x8C01674Eu;
    }
    else if (type & 0x2000) {
        angles = 0;
        quaternion = 0x8C01679Cu;
    }
    set(56, angles);
    set(64, quaternion);
    set(68, short_angles);
    set(60, type & 4 ? kFloat3 : kNone);
    // With bit 3 or 6 of the type set, the first node starts at its second channel.
    if (type & 72)
        set(28, word(word(kMotion) + 28) + 1);
    guest::store<uint32_t>(kEventFlag, type & 0x8000 ? 1 : 0);
    guest::store<uint16_t>(kEvent, 0);
}

// Draws `object` in `motion_link` at the frame `rate`, each model with `pDrawFunc`.
void njDrawMotionLink(gptr<NJS_CNK_OBJECT> object, gptr<NJS_MOTION_LINK> motion_link, Float rate,
                      gptr<code> pDrawFunc) {
    diff::Check check("njDrawMotionLink", 0x8C017418u, object, motion_link, rate, pDrawFunc);
    njStartMotionObj(gptr<NJS_MOTION>(motion_link.a), rate);
    guest::store<uint32_t>(word(0x8C0CD0A0u) + 12, pDrawFunc.a);
    _njPushPopMotionLink(object);
}

// The vertex list `vlist` of a shape motion's node for the vertex type `vtype`, blended from the
// shape's two keys around the frame (0x8C10B540 points to the shape state, 0x8C10B544 to its keys,
// 0x8C10B548 to the shape lists). Each part that the type has and the keys hold, positions and
// normals as three floats and colours as four bytes, is (1 - t) times the first key's plus t times
// the second's; a part that the keys do not hold keeps its place in `vlist` unchanged.
void njSetShapeVlist(gptr<Uint32> vlist, Uint32 vtype) {
    diff::Check check("njSetShapeVlist", 0x8C01785Cu, vlist, vtype);
    const uint32_t info = word(0x8C10B540u), entry = word(0x8C10B544u) + 16 * word(info + 24);
    const uint32_t lists = word(word(0x8C10B548u));
    const uint32_t index = 4u * guest::load<uint16_t>(word(info + 16) + 6);
    uint32_t a = word(word(lists + 8 * word(entry + 4)) + index);
    uint32_t b = word(word(lists + 8 * word(entry + 8)) + index);
    // The parts of the type: positions (1), normals (2), colours (4) and a fourth word (8).
    uint32_t parts;
    switch (vtype & 0xFF) {
        case 0x22: parts = 1; break;
        case 0x23: parts = 5; break;
        case 0x24: parts = 9; break;
        case 0x29: parts = 3; break;
        case 0x2A: parts = 7; break;
        case 0x2B:
        case 0x2C: parts = 11; break;
        default: return;
    }
    // The parts that the keys hold (16, 32, 64), and the words of each of their vertices.
    int32_t words;
    switch (guest::load<uint16_t>(a) & 0xFF) {
        case 0x22: parts |= 0x10, words = 3; break;
        case 0x23: parts |= 0x50, words = 4; break;
        case 0x29: parts |= 0x30, words = 6; break;
        case 0x2A: parts |= 0x70, words = 7; break;
        case 0x82: parts |= 0x40, words = 1; break;
        case 0x80: parts |= 0x20, words = 3; break;
        case 0x81: parts |= 0x60, words = 4; break;
        default: return;
    }
    const uint32_t packed = (parts << 16) | static_cast<uint32_t>(_divls(words, guest::load<uint16_t>(a + 2)));
    parts = packed >> 16;
    uint32_t count = packed & 0xFFFF, out = vlist.a;
    const float t = real(entry + 12), s = 1.0f - t;
    a += 4;
    b += 4;
    do {
        for (uint32_t part = 1; part <= 2; part <<= 1) {
            if (!(parts & part))
                continue;
            if (parts & (part << 4)) {
                for (uint32_t i = 0; i < 12; i += 4)
                    guest::store<float>(out + i, std::fma(s, real(a + i), real(b + i) * t));
                a += 12;
                b += 12;
            }
            out += 12;
        }
        if (parts & 4) {
            if (parts & 64) {
                for (uint32_t k = 0; k < 4; ++k) {
                    const float from = static_cast<float>(guest::load<uint8_t>(a + k));
                    const float to = static_cast<float>(guest::load<uint8_t>(b + k));
                    guest::store<uint8_t>(out + k, static_cast<uint8_t>(cpu::ftrc(std::fma(s, from, to * t))));
                }
                a += 4;
                b += 4;
            }
            out += 4;
        }
        if (parts & 8)
            out += 4;
    } while (--count != 0);
}

// njDrawMotionLink with the draw functions of the chunk models: multi-light Easy models,
// FUN_8c01a23a's models, multi-light Simple models and Simple models.
void FUN_8c016118(Float param_1, gptr<NJS_CNK_OBJECT> param_2, gptr<NJS_MOTION_LINK> param_3) {
    diff::Check check("FUN_8c016118", 0x8C016118u, param_1, param_2, param_3);
    njDrawMotionLink(param_2, param_3, param_1, gptr<code>(0x8C01C596u));
}
void FUN_8c016128(Float param_1, gptr<NJS_CNK_OBJECT> param_2, gptr<NJS_MOTION_LINK> param_3) {
    diff::Check check("FUN_8c016128", 0x8C016128u, param_1, param_2, param_3);
    njDrawMotionLink(param_2, param_3, param_1, gptr<code>(0x8C01A23Au));
}
void FUN_8c016138(Float param_1, gptr<NJS_CNK_OBJECT> param_2, gptr<NJS_MOTION_LINK> param_3) {
    diff::Check check("FUN_8c016138", 0x8C016138u, param_1, param_2, param_3);
    njDrawMotionLink(param_2, param_3, param_1, gptr<code>(0x8C029772u));
}
void FUN_8c016148(Float param_1, gptr<NJS_CNK_OBJECT> param_2, gptr<NJS_MOTION_LINK> param_3) {
    diff::Check check("FUN_8c016148", 0x8C016148u, param_1, param_2, param_3);
    njDrawMotionLink(param_2, param_3, param_1, gptr<code>(0x8C01CF3Eu));
}

// njCnkDrawShapeMotion with the shape at the motion's frame and multi-light Easy models.
void njCnkEasyMultiDrawShapeMotion(gptr<NJS_CNK_OBJECT> object, gptr<NJS_MOTION> motion, gptr<NJS_MOTION> shape,
                                   Float frame) {
    diff::Check check("njCnkEasyMultiDrawShapeMotion", 0x8C016158u, object, motion, shape, frame);
    njCnkDrawShapeMotion(object, motion, shape, frame, frame, gptr<code>(0x8C01C596u));
}

// A motion callback that does nothing.
undefined4 FUN_8c0173dc() {
    diff::Check check("FUN_8c0173dc", 0x8C0173DCu);
    return check.returns(undefined4(0));
}

// A camera's motion into the four places that `param_3` points to: the position, the vector, the
// roll and the angle of view, from the camera `param_1` and the motion `param_2` at the frame
// `arg_fr4`. The motion's flags (32 in its state) tell its channels; the ones a camera does not use
// are skipped. A part without keys takes the camera's own, but a roll without a channel stays as it
// was. With flag 64, the vector is the motion's point less the position.
void FUN_8c016170(gptr<float> param_1, gptr<NJS_MOTION> param_2, gptr<int> param_3, float arg_fr4, float arg_fr5,
                  undefined4 arg_r7, float arg_fr6, float arg_fr7) {
    diff::Check check("FUN_8c016170", 0x8C016170u, param_1, param_2, param_3, arg_fr4, arg_fr5, arg_r7, arg_fr6,
                      arg_fr7);
    const uint32_t camera = param_1.a, out = param_3.a;
    const uint32_t pos = word(out), vec = word(out + 4), roll = word(out + 8), ang = word(out + 12);
    njSetCurMotionInfoSlot(0);
    njStartMotionEx(param_2, arg_fr4);
    const uint32_t flags = word(word(kMotion) + 32);
    const auto skip = [] {
        const uint32_t motion = word(kMotion);
        guest::store<uint32_t>(motion + 28, word(motion + 28) + 1);
    };
    const auto point = [](uint32_t at) { return _njGetMotionFloat3(at, 0, 0, 0, 0, 0, 0, 0) != 0u; };
    if (!(flags & 1) || !point(pos))
        copy(pos, camera, 3);
    if (flags & 0x2022)
        skip();
    if (flags & 4)
        skip();
    bool keyed = false;
    if (flags & 64) {
        keyed = point(vec);
        if (keyed)
            for (uint32_t i = 0; i < 12; i += 4) guest::store<float>(vec + i, real(vec + i) - real(pos + i));
    }
    else if (flags & 8) {
        keyed = point(vec);
    }
    if (!keyed)
        copy(vec, camera + 12, 3);
    if (flags & 0x8000)
        skip();
    if ((flags & 0x80) && !_njGetMotionSangle1(gptr<undefined2>(roll), 0, 0))
        guest::store<uint16_t>(roll, guest::load<uint16_t>(camera + 24));
    if (!(flags & 0x100) || !_njGetMotionSangle1(gptr<undefined2>(ang), 0, 0))
        guest::store<uint16_t>(ang, guest::load<uint16_t>(camera + 26));
}

// A camera motion as the view: FUN_8c016170 into a camera on the stack with the type of `param_1`,
// then njSetCamera. The machine code keeps that camera in its frame, 52 bytes below the stack
// pointer, and so does this: a roll that the motion leaves out is what the frame held.
void FUN_8c0162a8(gptr<float> param_1, gptr<NJS_MOTION> param_2, float arg_fr4, float arg_fr5, undefined4 arg_r6,
                  undefined4 arg_r7, float arg_fr6, float arg_fr7) {
    diff::Check check("FUN_8c0162a8", 0x8C0162A8u, param_1, param_2, arg_fr4, arg_fr5, arg_r6, arg_r7, arg_fr6,
                      arg_fr7);
    guest::Frame frame(52);
    const uint32_t places = frame.top - 52, camera = places + 16;
    const uint32_t at[4] = {camera, camera + 12, camera + 24, camera + 26};
    for (uint32_t i = 0; i < 4; ++i) guest::store<uint32_t>(places + 4 * i, at[i]);
    guest::store<uint32_t>(camera + 28, word(param_1.a + 28));
    FUN_8c016170(param_1, param_2, gptr<int>(places), arg_fr4, 0, 0, 0, 0);
    njSetCamera(gptr<NJS_CAMERA>(camera));
}

namespace {

// The shape motions' state: 0x8C10B540 points to the current one of the slots at 0x8C10B508, 28
// bytes each. A slot holds the current node (0), the frame (4), the shape's second word (8), the
// node's count of vertex chunks (12), the current chunk's key list (16; 8 bytes each: the keys and
// their count), the count of started shapes (20) and the current chunk's group (24). 0x8C10B544
// points to the groups' keys, 16 bytes each, 0x8C10B548 to the shape lists, and 0x8C10B54C holds
// the motion that goes with the shape.
constexpr uint32_t kShape = 0x8C10B540u, kShapeGroups = 0x8C10B544u, kShapeLists = 0x8C10B548u;
constexpr uint32_t kShapeMotion = 0x8C10B54Cu;

uint32_t shape() { return word(kShape); }

}  // namespace

void njSetNextShapeNodeEx() {
    diff::Check check("njSetNextShapeNodeEx", 0x8C0174F0u);
    guest::store<uint32_t>(shape(), word(shape()) + 8);
    guest::store<uint32_t>(shape() + 12, word(word(shape()) + 4));
    guest::store<uint32_t>(shape() + 16, word(word(shape())));
}

void njStartShapeEx(gptr<NJS_MOTION> shape_motion, Float frame) {
    diff::Check check("njStartShapeEx", 0x8C017510u, shape_motion, frame);
    guest::store<uint32_t>(shape(), word(shape_motion.a));
    guest::store<uint32_t>(shape() + 8, word(shape_motion.a + 4));
    guest::store<float>(shape() + 4, frame);
    guest::store<uint32_t>(shape() + 20, word(shape() + 20) + 1);
    guest::store<uint32_t>(shape() + 12, word(word(shape()) + 4));
    guest::store<uint32_t>(shape() + 16, word(word(shape())));
}

void njSetCurShapeInfoSlot(int Slot) {
    diff::Check check("njSetCurShapeInfoSlot", 0x8C017548u, Slot);
    guest::store<uint32_t>(kShape, static_cast<uint32_t>(Slot) * 28 + 0x8C10B508u);
}

// The groups' keys go to `buf`. Ninja clears only its first word, once for each 16 bytes of `size`.
void njInitShape(gptr<void> buf, Sint32 size) {
    diff::Check check("njInitShape", 0x8C017558u, buf, size);
    guest::store<uint32_t>(kShapeGroups, buf.a);
    if (static_cast<int32_t>(static_cast<uint32_t>(size) >> 4) > 0)
        guest::store<uint32_t>(buf.a, 0);
}

void njSetShapeList(gptr<NJS_CSHAPELIST> shape_list) {
    diff::Check check("njSetShapeList", 0x8C017576u, shape_list);
    guest::store<uint32_t>(kShapeLists, shape_list.a);
}

// The keys around the frame for the current chunk's group, once for each started shape: the group
// comes from the shape list entry that the chunk's first key names.
void njGetShapeGroupID() {
    diff::Check check("njGetShapeGroupID", 0x8C0175CCu);
    const uint32_t chunk = word(shape() + 16);
    const uint32_t entry = word(word(kShapeLists)) + 8 * word(word(chunk) + 4);
    const uint32_t group = guest::load<uint16_t>(entry + 4);
    guest::store<uint32_t>(shape() + 24, group);
    const uint32_t record = word(kShapeGroups) + 16 * group;
    if (word(record) == word(shape() + 20))
        return;
    const Keys k = keys32(word(chunk), 8, guest::load<uint16_t>(chunk + 4), real(shape() + 4));
    guest::store<uint32_t>(word(kShapeGroups) + 16 * group, word(shape() + 20));
    guest::store<uint32_t>(word(kShapeGroups) + 16 * group + 4, word(k.from + 4));
    guest::store<uint32_t>(word(kShapeGroups) + 16 * group + 8, word(k.to + 4));
    guest::store<float>(word(kShapeGroups) + 16 * group + 12, k.t);
}

// The model's vertex chunks, each blended by njSetShapeVlist with the keys of its group.
void njCalcShapeVlist(gptr<NJS_CNK_MODEL> cnkmodel) {
    diff::Check check("njCalcShapeVlist", 0x8C01757Cu, cnkmodel);
    uint32_t at = word(cnkmodel.a);
    for (int32_t i = 0; i < guest::load<int32_t>(shape() + 12); ++i) {
        njGetShapeGroupID();
        const uint32_t type = guest::load<uint16_t>(at) & 0xFF, size = guest::load<uint16_t>(at + 2);
        at += 8;
        njSetShapeVlist(gptr<Uint32>(at), type);
        guest::store<uint32_t>(shape() + 16, word(shape() + 16) + 8);
        at += size * 4 - 4;
    }
}

// An object tree with a shape motion: as _njPushPopMotionLink, with each model's vertex chunks
// blended first (njCalcShapeVlist) when the shape has keys, and the shape's next node for each
// object without flag 128. Without a motion, each object moves, turns and scales by its own values,
// as its flags allow.
void _njCnkPushPopShape(gptr<NJS_CNK_OBJECT> param_1) {
    diff::Check check("_njCnkPushPopShape", 0x8C0176D8u, param_1);
    guest::Frame frame(56);
    const uint32_t saved = frame.top - 56;
    uint32_t object = param_1.a;
    do {
        njPushMatrixEx();
        if (word(kShapeMotion)) {
            njMotionTransformEx(gptr<NJS_CNK_OBJECT>(object));
            if (word(kEventFlag)) {
                _quick_evn_mvn(56, gptr<void>(saved), gptr<void>(object));
                njChangeMotionObject(gptr<NJS_CNK_OBJECT>(object));
            }
        }
        else {
            if (!(word(object) & 1))
                njTranslateEx(gptr<NJS_VECTOR>(object + kPosition));
            if (!(word(object) & 2)) {
                if (word(object) & 0x400)
                    njQuaternionEx2(real(object + kReal), gptr<Float>(object + kAngles));
                else
                    njRotateEx(gptr<Angle>(object + kAngles), static_cast<Sint32>(word(object) & 32));
            }
            if (!(word(object) & 4))
                njScaleEx(gptr<NJS_VECTOR>(object + kScale));
        }
        if (const uint32_t callback = word(0x8C0CD078u))
            call(callback, {object});
        const uint32_t model = word(object + 4);
        const bool drawn = model && !(word(object) & 8);
        if (!(word(object) & 128)) {
            if (drawn) {
                if (word(word(shape())))
                    njCalcShapeVlist(gptr<NJS_CNK_MODEL>(model));
                call(word(word(0x8C0CD0A0u) + 12), {model});
            }
            njSetNextShapeNodeEx();
        }
        else if (drawn) {
            call(word(word(0x8C0CD0A0u) + 12), {model});
        }
        if (word(kShapeMotion) && word(kEventFlag))
            _quick_evn_mvn(56, gptr<void>(object), gptr<void>(saved));
        if (word(object + kChild))
            _njCnkPushPopShape(gptr<NJS_CNK_OBJECT>(word(object + kChild)));
        njPopMatrixEx();
        object = word(object + kSibling);
    } while (object);
}

// An object tree with a motion and a shape motion, drawn with `pDrawFunc`; without a shape, as
// _njPushPopMotionLink.
void njCnkDrawShapeMotion(gptr<NJS_CNK_OBJECT> object, gptr<NJS_MOTION> motion, gptr<NJS_MOTION> shape_motion,
                          Float frame, Float sframe, gptr<code> pDrawFunc) {
    diff::Check check("njCnkDrawShapeMotion", 0x8C017684u, object, motion, shape_motion, frame, sframe, pDrawFunc);
    guest::store<uint32_t>(kShapeMotion, motion.a);
    if (motion.a)
        njStartMotionObj(motion, frame);
    guest::store<uint32_t>(word(0x8C0CD0A0u) + 12, pDrawFunc.a);
    if (!shape_motion.a) {
        _njPushPopMotionLink(object);
        return;
    }
    njSetCurShapeInfoSlot(0);
    njStartShapeEx(shape_motion, sframe);
    _njCnkPushPopShape(object);
}

}  // namespace game
