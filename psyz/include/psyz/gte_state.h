#ifndef PSYZ_GTE_STATE_H
#define PSYZ_GTE_STATE_H
#include <string.h>

/**
 * @file gte_state.h
 * @brief The GTE's registers, and inline access to them.
 *
 * Games reach the GTE through Psyz_GteDataWrite/Read, Psyz_GteCtrlWrite/Read
 * and Psyz_GteCommand, almost always with a constant register or command:
 * gte.h turns those calls into the inline versions here, which compile to
 * plain loads and stores (and direct calls for the common commands). Any
 * other call still goes to libgte.c. Both behave the same.
 *
 * It needs none of the SDK's types (a game may declare its own): with
 * libgte.h included first the registers use SVECTOR, CVECTOR and MATRIX,
 * otherwise types of the same layout.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LIBGTE_H
typedef SVECTOR PsyzGteSvector;
typedef CVECTOR PsyzGteCvector;
typedef MATRIX PsyzGteMatrix;
#else
typedef struct {
    short vx, vy, vz, pad;
} PsyzGteSvector;
typedef struct {
    unsigned char r, g, b, cd;
} PsyzGteCvector;
typedef struct {
    short m[3][3];
    int t[3];
} PsyzGteMatrix;
#endif

// Names from https://www.problemkaputt.de/psx-spx.htm#gteoverview
#define GTE_REGS(R)                                                            \
    R(PsyzGteSvector, v0)         /* cop1 0-1 */                                      \
    R(PsyzGteSvector, v1)         /* cop1 2-3 */                                      \
    R(PsyzGteSvector, v2)         /* cop1 4-5 */                                      \
    R(PsyzGteCvector, rgbc)       /* cop1 6 */                                        \
    R(unsigned short, otz) /* cop1 7 average Z value */                        \
    R(short, ir0)          /* cop1 8 accumulator, interpolate */               \
    R(short, ir1)          /* cop1 9 accumulator, vector x */                  \
    R(short, ir2)          /* cop1 10 accumulator, vector y */                 \
    R(short, ir3)          /* cop1 11 accumulator, vector z */                 \
    R(short, sx0)          /* cop1 12-14 screen XY FIFO */                     \
    R(short, sy0)                                                              \
    R(short, sx1)                                                              \
    R(short, sy1)                                                              \
    R(short, sx2)                                                              \
    R(short, sy2)                                                              \
    R(short, sxp) /* cop1 15 */                                                \
    R(short, syp)                                                              \
    R(unsigned short, sz0) /* cop1 16-19 screen Z FIFO */                      \
    R(unsigned short, sz1)                                                     \
    R(unsigned short, sz2)                                                     \
    R(unsigned short, sz3)                                                     \
    R(int, mac0)          /* cop1 24 math accumulator (value) */               \
    R(int, mac1)          /* cop1 25 math accumulator (vector) */              \
    R(int, mac2)          /* cop1 26 math accumulator (vector) */              \
    R(int, mac3)          /* cop1 27 math accumulator (vector) */              \
    R(unsigned int, rgb0) /* cop1 20 color FIFO */                             \
    R(unsigned int, rgb1) /* cop1 21 */                                        \
    R(unsigned int, rgb2) /* cop1 22 */                                        \
    R(unsigned int, res1) /* cop1 23 (reserved) */                             \
    R(unsigned int, lzcs) /* cop1 30 leading sign source */                    \
    R(unsigned int, lzcr) /* cop1 31 leading sign count */                     \
    R(PsyzGteMatrix, m)          /* cop2 0-7, rotation 3x3 + translation */           \
    R(PsyzGteMatrix, l1)         /* cop2 8-15 light source 3x3 + bg color */          \
    R(PsyzGteMatrix, l2)         /* cop2 16-23 light source 3x3 + bg color */         \
    R(int, ofx)           /* cop2 24 screen offset X */                        \
    R(int, ofy)           /* cop2 25 screen offset Y */                        \
    R(unsigned short, h)  /* cop2 26 projection plane distance */              \
    R(short, dqa)         /* cop2 27 depth queing parameter A (coeff) */       \
    R(int, dqb)           /* cop2 28 depth queing parameter B (s32) */         \
    R(short, zsf3)        /* cop2 29 average Z scale factor */                 \
    R(short, zsf4)        /* cop2 30 average Z scale factor */                 \
    R(unsigned int, flag) /* cop2 31 */

#define PSYZ_GTE_REG(type, name) type name;
typedef struct {
    GTE_REGS(PSYZ_GTE_REG)
} PsyzGteRegs;
#undef PSYZ_GTE_REG

extern PsyzGteRegs psyz_gte;

#define PSYZ_GTE_LO(v) ((short)((v) & 0xFFFF))
#define PSYZ_GTE_HI(v) ((short)(((v) >> 16) & 0xFFFF))
#define PSYZ_GTE_PAIR(a, b)                                                    \
    (((unsigned int)(unsigned short)(a)) |                                     \
     (((unsigned int)(unsigned short)(b)) << 16))

void Psyz_GteDataWrite(unsigned reg, unsigned int value);
unsigned int Psyz_GteDataRead(unsigned reg);
void Psyz_GteCtrlWrite(unsigned reg, unsigned int value);
unsigned int Psyz_GteCtrlRead(unsigned reg);

// as Psyz_GteDataWrite (LZCS goes there: it counts bits)
static inline void Psyz_GteDataWriteInline(unsigned idx, unsigned int v) {
    PsyzGteRegs* g = &psyz_gte;
    switch (idx) {
    case 0: g->v0.vx = PSYZ_GTE_LO(v); g->v0.vy = PSYZ_GTE_HI(v); break;
    case 1: g->v0.vz = PSYZ_GTE_LO(v); break;
    case 2: g->v1.vx = PSYZ_GTE_LO(v); g->v1.vy = PSYZ_GTE_HI(v); break;
    case 3: g->v1.vz = PSYZ_GTE_LO(v); break;
    case 4: g->v2.vx = PSYZ_GTE_LO(v); g->v2.vy = PSYZ_GTE_HI(v); break;
    case 5: g->v2.vz = PSYZ_GTE_LO(v); break;
    case 6: memcpy(&g->rgbc, &v, 4); break;
    case 7: g->otz = (unsigned short)(v & 0xFFFF); break;
    case 8: g->ir0 = PSYZ_GTE_LO(v); break;
    case 9: g->ir1 = PSYZ_GTE_LO(v); break;
    case 10: g->ir2 = PSYZ_GTE_LO(v); break;
    case 11: g->ir3 = PSYZ_GTE_LO(v); break;
    case 12: g->sx0 = PSYZ_GTE_LO(v); g->sy0 = PSYZ_GTE_HI(v); break;
    case 13: g->sx1 = PSYZ_GTE_LO(v); g->sy1 = PSYZ_GTE_HI(v); break;
    case 14: g->sx2 = PSYZ_GTE_LO(v); g->sy2 = PSYZ_GTE_HI(v); break;
    case 15: g->sxp = PSYZ_GTE_LO(v); g->syp = PSYZ_GTE_HI(v); break;
    case 16: g->sz0 = (unsigned short)(v & 0xFFFF); break;
    case 17: g->sz1 = (unsigned short)(v & 0xFFFF); break;
    case 18: g->sz2 = (unsigned short)(v & 0xFFFF); break;
    case 19: g->sz3 = (unsigned short)(v & 0xFFFF); break;
    case 20: g->rgb0 = v; break;
    case 21: g->rgb1 = v; break;
    case 22: g->rgb2 = v; break;
    case 23: g->res1 = v; break;
    case 24: g->mac0 = (int)v; break;
    case 25: g->mac1 = (int)v; break;
    case 26: g->mac2 = (int)v; break;
    case 27: g->mac3 = (int)v; break;
    default: (Psyz_GteDataWrite)(idx, v); break;
    }
}

static inline unsigned int Psyz_GteDataReadInline(unsigned idx) {
    const PsyzGteRegs* g = &psyz_gte;
    unsigned int w;
    switch (idx) {
    case 0: return PSYZ_GTE_PAIR(g->v0.vx, g->v0.vy);
    case 1: return (unsigned int)(int)g->v0.vz;
    case 2: return PSYZ_GTE_PAIR(g->v1.vx, g->v1.vy);
    case 3: return (unsigned int)(int)g->v1.vz;
    case 4: return PSYZ_GTE_PAIR(g->v2.vx, g->v2.vy);
    case 5: return (unsigned int)(int)g->v2.vz;
    case 6: memcpy(&w, &g->rgbc, 4); return w;
    case 7: return (unsigned int)g->otz;
    case 8: return (unsigned int)(int)g->ir0;
    case 9: return (unsigned int)(int)g->ir1;
    case 10: return (unsigned int)(int)g->ir2;
    case 11: return (unsigned int)(int)g->ir3;
    case 12: return PSYZ_GTE_PAIR(g->sx0, g->sy0);
    case 13: return PSYZ_GTE_PAIR(g->sx1, g->sy1);
    case 14: return PSYZ_GTE_PAIR(g->sx2, g->sy2);
    case 15: return PSYZ_GTE_PAIR(g->sxp, g->syp);
    case 16: return (unsigned int)g->sz0;
    case 17: return (unsigned int)g->sz1;
    case 18: return (unsigned int)g->sz2;
    case 19: return (unsigned int)g->sz3;
    case 20: return g->rgb0;
    case 21: return g->rgb1;
    case 22: return g->rgb2;
    case 23: return g->res1;
    case 24: return (unsigned int)g->mac0;
    case 25: return (unsigned int)g->mac1;
    case 26: return (unsigned int)g->mac2;
    case 27: return (unsigned int)g->mac3;
    default: return (Psyz_GteDataRead)(idx);
    }
}

// a 3x3 matrix and its translation: control registers base..base+7
static inline void Psyz_GteMatrixWrite(PsyzGteMatrix* m, unsigned idx, unsigned int v) {
    switch (idx) {
    case 0: m->m[0][0] = PSYZ_GTE_LO(v); m->m[0][1] = PSYZ_GTE_HI(v); break;
    case 1: m->m[0][2] = PSYZ_GTE_LO(v); m->m[1][0] = PSYZ_GTE_HI(v); break;
    case 2: m->m[1][1] = PSYZ_GTE_LO(v); m->m[1][2] = PSYZ_GTE_HI(v); break;
    case 3: m->m[2][0] = PSYZ_GTE_LO(v); m->m[2][1] = PSYZ_GTE_HI(v); break;
    case 4: m->m[2][2] = PSYZ_GTE_LO(v); break;
    default: m->t[idx - 5] = (int)v; break;
    }
}

static inline void Psyz_GteCtrlWriteInline(unsigned idx, unsigned int v) {
    if (idx < 8) {
        Psyz_GteMatrixWrite(&psyz_gte.m, idx, v);
    } else if (idx < 16) {
        Psyz_GteMatrixWrite(&psyz_gte.l1, idx - 8, v);
    } else if (idx < 24) {
        Psyz_GteMatrixWrite(&psyz_gte.l2, idx - 16, v);
    } else {
        (Psyz_GteCtrlWrite)(idx, v);
    }
}

void Psyz_GteRtps(void);
void Psyz_GteRtpt(void);
void Psyz_GteNclip(void);
void Psyz_GteAvsz3(void);
void Psyz_GteAvsz4(void);
void Psyz_GteRt(void);
void Psyz_GteCommand(unsigned int cmd);

// as Psyz_GteCommand: the common commands called directly
static inline void Psyz_GteCommandInline(unsigned int cmd) {
    switch (cmd & 0x1FFFFFF) {
    case 0x0180001: Psyz_GteRtps(); break;   // RTPS sf=1
    case 0x0280030: Psyz_GteRtpt(); break;   // RTPT sf=1
    case 0x1400006: Psyz_GteNclip(); break;  // NCLIP
    case 0x158002D: Psyz_GteAvsz3(); break;  // AVSZ3
    case 0x168002E: Psyz_GteAvsz4(); break;  // AVSZ4
    case 0x0480012: Psyz_GteRt(); break;     // MVMVA rt*v0+tr, sf=1
    default: (Psyz_GteCommand)(cmd); break;
    }
}

#if (defined(__GNUC__) || defined(__clang__)) && !defined(PSYZ_GTE_NO_INLINE)
// A constant register or command (the usual case) takes the inline version
#define Psyz_GteDataWrite(reg, value)                                          \
    (__builtin_constant_p(reg) ? Psyz_GteDataWriteInline((reg), (value))       \
                               : (Psyz_GteDataWrite)((reg), (value)))
#define Psyz_GteDataRead(reg)                                                  \
    (__builtin_constant_p(reg) ? Psyz_GteDataReadInline(reg)                   \
                               : (Psyz_GteDataRead)(reg))
#define Psyz_GteCtrlWrite(reg, value)                                          \
    (__builtin_constant_p(reg) ? Psyz_GteCtrlWriteInline((reg), (value))       \
                               : (Psyz_GteCtrlWrite)((reg), (value)))
#define Psyz_GteCommand(cmd)                                                   \
    (__builtin_constant_p(cmd) ? Psyz_GteCommandInline(cmd)                    \
                               : (Psyz_GteCommand)(cmd))
#endif

#ifdef __cplusplus
}
#endif

#endif
