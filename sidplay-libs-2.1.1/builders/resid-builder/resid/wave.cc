//  ---------------------------------------------------------------------------
//  This file is part of reSID, a MOS6581 SID emulator engine.
//  Copyright (C) 2004  Dag Lem <resid@nimrod.no>
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//  ---------------------------------------------------------------------------

#include "wave.h"
#include "sid.h"

typedef struct {
    float bias;
    float pulsestrength;
    float topbit;
    float distance;
    float stmix;
} waveformconfig_t;

const float sharpness = 512.f;
const waveformconfig_t wfconfig[3][5] = {
  { /* kevtris chip D (6581r2/r3) */
    { 0.9634957f, 0.f, 0.f, 4.165269f, 0.8020396f }, // 240 bits wrong
    { 0.8931507f, 2.483499f, 1.0f, 0.03339716f, 0.0f }, // 600 bits wrong
    { 0.8869214f, 2.440879f, 1.680824f, 0.02267573f, 0.0f }, // 613 bits wrong
    { 0.9842906f, 2.772751f, 0.f, 0.4342486f, 1.f }, // 32 bits wrong
    { 0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
  },
  { /* kevtris chip G (6581r2/r3) */
    { 0.9506974f, 0.f, 0.f, 2.104169f, 0.7887034f }, // 188 bits wrong
    { 0.8924618f, 2.01122f, 1.0f, 0.03133072f, 0.0f }, // 360 bits wrong
    { 0.8952018f, 2.213601f, 1.705941f, 0.01260567f, 0.0f }, // 668 bits wrong
    { 0.9527834f, 1.794777f, 0.f, 0.09806272f, 0.7752482f }, // 12 bits wrong
    { 0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
  },
  { /* kevtris chip V (8580) */
    { 0.9781665f, 0.f, 0.9927619f, 3.705556f, 0.9659713f }, // 311 bits wrong
    { 0.9236207f, 2.19129f, 1.0f, 0.1108298f, 0.0f }, // 628 bits wrong
    { 0.9248214f, 2.232846f, 0.9491023f, 0.1313893f, 0.0f }, // 593 bits wrong
    { 0.9845552f, 1.381085f, 0.9621315f, 1.699522f, 1.f }, // 168 bits wrong
    { 0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
  },
};

/* render output from bitstate */
static float make_sample(float *o) {
  float out = 0;
  for (int i = 0; i < 12; i ++) {
    out += o[i] * dac[i];
  }
  return out;
}

/* generate tables for waveforms 1 .. 7 */
void WaveformGenerator::rebuild_wftable() {
    float o[12];
    reg8 oldwf = waveform;
    reg32 oldacc = accumulator;
    reg12 oldpw = pw;

    for (waveform = 1; waveform < 8; waveform ++) {
        for (accumulator = 0; accumulator < (1<<24); accumulator += (1<<12)) {
            /* generate pulse-low variants. Also,
             * when waveform < 4, pw doesn't matter. */
            pw = 0x1000; /* pulse always low */
            calculate_waveform_sample(o);
            wftable[waveform - 1][accumulator >> 12] = make_sample(o) + wave_zero;
            /* Add pulse-high variants after pulse-low state variants */
            if (waveform >= 4) {
                pw = 0x000; /* pulse always high */
                calculate_waveform_sample(o);
                wftable[waveform + 3][accumulator >> 12] = make_sample(o) + wave_zero;
            }
        }
    }

    waveform = oldwf;
    accumulator = oldacc;
    pw = oldpw;
}

/* explode reg12 to a floating point bit array */
static void populate(reg12 v, float o[12])
{
    int j = 1;
    for (int i = 0; i < 12; i ++) {
        o[i] = v & j ? 1.f : 0.f;
        j <<= 1;
    }
}

/* waveform values valid are 1 .. 7 */
void WaveformGenerator::calculate_waveform_sample(float o[12])
{
  int i;

  /* P */
  if (waveform == 4) {
    populate((accumulator >> 12) >= pw ? 0xfff : 0x000, o);
    return;
  }

  const waveformconfig_t config = wfconfig[
    model == MOS6581 ? 1 : 2
  ][
    waveform == 3 ? 0 :
    waveform == 5 ? 1 :
    waveform == 6 ? 2 :
    waveform == 7 ? 3 :
                    4
  ];

  /* S with strong top bit for 6581 */
  populate(accumulator >> 12, o);

  /* convert to T */
  if ((waveform & 3) == 1) {
    bool top = (accumulator & 0x800000) != 0;
    for (i = 11; i > 0; i --) {
      if (top) {
        o[i] = 1.0f - o[i-1];
      } else {
        o[i] = o[i-1];
      }
    }
    o[0] = 0;
  }

  /* convert to ST */
  if ((waveform & 3) == 3) {
    /* bottom bit is grounded via T waveform selector */
    o[0] *= config.stmix;
    for (i = 1; i < 12; i ++) {
      o[i] = o[i - 1] * (1.f - config.stmix) + o[i] * config.stmix;
    }
  }
  o[11] *= config.topbit;

  /* ST, P* waveform? */
  if (waveform == 3 || waveform > 4) {
    float distancetable[12 * 2 + 1];
    for (i = 0; i <= 12; i ++) {
        distancetable[12+i] = distancetable[12-i] = 1.f / (1.f + i * i * config.distance);
    }

    float pulse = (accumulator >> 12) >= pw ? 1.f : -1.f;
    pulse *= config.pulsestrength;

    float tmp[12];
    for (i = 0; i < 12; i ++) {
        float avg = 0;
        float n = 0;
        for (int j = 0; j < 12; j ++) {
            float weight = distancetable[i - j + 12];
            avg += o[j] * weight;
            n += weight;
        }
        /* pulse control bit */
        if (waveform > 4) {
            float weight = distancetable[i - 12 + 12];
            avg += pulse * weight;
            n += weight;
        }

        tmp[i] = (o[i] + avg / n) * 0.5f;
    }

    for (i = 0; i < 12; i ++) {
        o[i] = tmp[i];
    }
  }

  /* use the environment around bias value to set/clear dac bit. The
   * relationship is nonlinear because that seems to sound a bit better. */
  for (i = 0; i < 12; i ++) {
    o[i] = (o[i] - config.bias) * sharpness;

    o[i] += 0.5f;
    if (o[i] > 1.f) {
       o[i] = 1.f;
    }
    if (o[i] < 0.f) {
        o[i] = 0.;
    }
  }
}

void WaveformGenerator::set_nonlinearity(float nl)
{
  for (int i = 0; i < 12; i ++) {
    dac[i] = SID::kinked_dac((1 << i), nl, 12);
  }
}

// ----------------------------------------------------------------------------
// Constructor.
// ----------------------------------------------------------------------------
WaveformGenerator::WaveformGenerator()
{
  set_chip_model(MOS6581);
  reset();
}

// ----------------------------------------------------------------------------
// Set chip model.
// ----------------------------------------------------------------------------
void WaveformGenerator::set_chip_model(chip_model model)
{
  this->model = model;
  wave_zero = static_cast<float>(model == MOS6581 ? -0x380 : -0x800);
}


// ----------------------------------------------------------------------------
// Register functions.
// ----------------------------------------------------------------------------
void WaveformGenerator::writeFREQ_LO(reg8 freq_lo)
{
  freq = (freq & 0xff00) | (freq_lo & 0xff);
}

void WaveformGenerator::writeFREQ_HI(reg8 freq_hi)
{
  freq = ((freq_hi << 8) & 0xff00) | (freq & 0xff);
}

void WaveformGenerator::writePW_LO(reg8 pw_lo)
{
  pw = (pw & 0xf00) | (pw_lo & 0x0ff);
}

void WaveformGenerator::writePW_HI(reg8 pw_hi)
{
  pw = ((pw_hi << 8) & 0xf00) | (pw & 0x0ff);
}

void WaveformGenerator::writeCONTROL_REG(WaveformGenerator& source, reg8 control)
{
  /* when selecting the 0 waveform, the previous output is held for
   * a time in the DAC MOSFET gates. We keep on holding forever, though... */
  reg4 waveform_next = (control >> 4) & 0x0f;
  if (waveform_next == 0 && waveform >= 1 && waveform <= 7) {
    previous = readOSC(source);
    previous_dac = output(source);
  }

  waveform = waveform_next;
  ring_mod = (control & 0x04) != 0;
  sync = (control & 0x02) != 0;
  bool test_next = (control & 0x08) != 0;

  // Test bit rising? Invert bit 19 and write it to bit 1.
  if (test_next && !test) {
    accumulator = 0;
    reg24 bit19 = (shift_register >> 18) & 2;
    shift_register = (shift_register & 0x7ffffd) | (bit19^2);
    noise_overwrite_delay = 200000; /* 200 ms, probably too generous? */
  } else {
    if (! test_next) {
      // Test bit falling? clock noise once,
      // otherwise just emulate noise's combined waveforms.
      clock_noise(test);
    }
  }
 
  test = test_next;
}

reg8 WaveformGenerator::readOSC(WaveformGenerator& source)
{
  float o[12];

  if (waveform == 0 || waveform > 7) {
    return previous;
  }

  /* Include effects of the test bit & ring mod */
  reg12 oldpw = pw;
  if (test) {
    pw = 0;
  }
  reg24 oldaccumulator = accumulator;
  accumulator ^= (waveform & 3) == 1 && ring_mod && (source.accumulator & 0x800000) ? 0x800000 : 0;
  calculate_waveform_sample(o);
  pw = oldpw;
  accumulator = oldaccumulator;

  reg8 out = 0;
  reg8 bit = 1;
  for (int i = 4; i < 12; i ++) {
    if (o[i] > 0.5f) {
        out |= bit;
    }
    bit <<= 1;
  }
  return out;
}

void WaveformGenerator::clock_noise(const bool clock)
{
  if (clock) {
    reg24 bit0 = ((shift_register >> 22) ^ (shift_register >> 17)) & 0x1;
    shift_register <<= 1;
    shift_register |= bit0;
  }

  // clear output bits of shift register if noise and other waveforms
  // are selected simultaneously
  if (waveform > 8) {
    shift_register &= 0x7fffff^(1<<22)^(1<<20)^(1<<16)^(1<<13)^(1<<11)^(1<<7)^(1<<4)^(1<<2);
  }

  if (waveform >= 8) {
    previous = outputN___() >> 4;
    previous_dac = wave_zero;
    for (int i = 0; i < 8; i ++) {
      if (previous & (1 << i)) {
        previous_dac += dac[i+4];
      }
    }
  }
}

reg12 WaveformGenerator::outputN___()
{
  return
    ((shift_register & 0x400000) >> 11) |
    ((shift_register & 0x100000) >> 10) |
    ((shift_register & 0x010000) >> 7) |
    ((shift_register & 0x002000) >> 5) |
    ((shift_register & 0x000800) >> 4) |
    ((shift_register & 0x000080) >> 1) |
    ((shift_register & 0x000010) << 1) |
    ((shift_register & 0x000004) << 2);
}

// ----------------------------------------------------------------------------
// SID reset.
// ----------------------------------------------------------------------------
void WaveformGenerator::reset()
{
  accumulator = 0;
  previous = 0;
  previous_dac = 0;
  shift_register = 0x7ffffc;
  freq = 0;
  pw = 0;
  test = 0;
  waveform = 0;
  writeCONTROL_REG(*this, 0);
  msb_rising = false;
}
