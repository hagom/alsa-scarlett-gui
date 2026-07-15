// SPDX-FileCopyrightText: 2022-2026 Geoffrey D. Bennett <g@b4.vu>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tooltips.h"

// tooltips that are used from multiple files

const char *level_descr =
  "Mic/Line or Instrument Level (Impedance)";

const char *air_descr =
  "Enabling Air will transform your recordings and inspire you while "
  "making music.";

const char *air_isa_descr =
  "Enabling Air engages an analogue high-shelf circuit which models "
  "the impedance and resonance characteristics of classic "
  "transformer-based ISA mic preamps, adding clarity and definition "
  "in the mid-frequency range. Great on most sources, particularly "
  "vocals and acoustic instruments.";

const char *air_430_descr =
  "430 Air is a recreation of the ISA 430 MkII’s Mic Air mode. Its "
  "inductor-based circuit interacts with the preamp to enhance the "
  "signal’s high-frequency content, adding clarity and a sense of "
  "spaciousness (“air”) without the need for EQ.";

const char *phantom_descr =
  "Enabling 48V sends “Phantom Power” to the XLR microphone input. "
  "This is required for some microphones (such as condensor "
  "microphones), and damaging to some microphones (particularly "
  "vintage ribbon microphones).";
