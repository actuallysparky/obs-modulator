// SPDX-FileCopyrightText: 2026 Sparky and OBS Modulator contributors
// SPDX-License-Identifier: GPL-2.0-or-later
uniform float gain < string label="Gain"; string widget_type="slider"; float minimum=0; float maximum=1; float step=0.001; > = 0.5;
uniform float other < string label="Other"; float minimum=0; float maximum=1; > = 0.5;
// Optional custom-host macro; stock Shaderfilter treats this as an ordinary
// numeric uniform. The generic callback fixture runs on every supported host.
uniform float gesture < string label="Gesture"; string widget_type="macro"; string ui_page="simple";
 string ui_curve='{"points":[{"position":0,"gain":0},{"position":100,"gain":1}]}';
 float minimum=0; float maximum=100; float step=0.1; > = 37;
float4 mainImage(VertData v_in) : TARGET { return image.Sample(textureSampler,v_in.uv)*float4(gain,gain,gain,1); }
