/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Shaders.h"

#include <stdexcept>
#include <string>

NATRON_NAMESPACE_ENTER

std::string
composeViewerFragmentShader(const std::string& ocioShaderText)
{
    // OCIO's GLSL 1.2 text needs 1.20 features (array constructors for its LUT tables); without
    // a #version line the driver compiles it as GLSL 1.10 and rejects it.
    std::string source = "#version 120\n"
                         "uniform sampler2D Tex;\n"
                         "uniform float gain;\n"
                         "uniform float offset;\n"
                         "uniform float gamma;\n"
                         "\n";
    source += ocioShaderText;
    source += "\n"
              "void main() {\n"
              "    vec4 c = texture2D(Tex, gl_TexCoord[0].st);\n"
              "    c.rgb = c.rgb * gain + offset;\n"
              "    c = OCIODisplay(c);\n"
              "    if (gamma <= 0.) {\n"
              "        c.rgb = vec3(greaterThanEqual(c.rgb, vec3(1.)));\n"
              "    } else {\n"
              "        c.rgb = pow(max(c.rgb, vec3(0.)), vec3(1. / gamma));\n"
              "    }\n"
              "    gl_FragColor = c;\n"
              "}\n";

    return source;
}

const char* vertRGB =
    "void main()\n"
    "{\n"
    "   gl_TexCoord[0] = gl_MultiTexCoord0;"
    "	gl_Position = ftransform();\n"
    "}\n"
    "\n"
;

/*There's a black texture used for when the user disconnect the viewer
   It's not just a shader,because we still need coordinates feedback.
 */
const char* blackFrag =
    "uniform sampler2D Tex;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(Tex,gl_TexCoord[0].st);\n"
    "}\n";
const char *histogramComputation_frag =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect Tex;\n"
    "uniform int channel;\n"
    "void main()\n"
    "{\n"
    "    gl_FragColor = vec4(1.0,0.0,0.0,1.0);\n"
    "}\n"
;
const char *histogramComputationVertex_vert =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect Tex;\n"
    "uniform int channel;\n"
    "attribute vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "\n"
    "    vec4 c = texture2DRect(Tex, TexCoord.xy  );\n"
    "\n"
    "    float sel = 0.0;\n"
    "    if(channel == 0){ // luminance\n"
    "     sel = 0.299*c.r + 0.587*c.g +0.114*c.b;\n"
    "    }else if(channel == 1){ // red\n"
    "            sel = c.r;\n"
    "    }else if(channel == 2){ //green\n"
    "            sel = c.g;\n"
    "    }else if(channel == 3){ // blue\n"
    "            sel = c.b;\n"
    "    }else if(channel == 4){ // alpha\n"
    "            sel = c.a;\n"
    "    }\n"
    "    clamp(sel, 0.0, 1.0);\n"
    "// set new point position to the color intensity in [-1.0,1.0] interval\n"
    "// as this is homogeneous coord. clip space\n"
    "    gl_Position.x =(2.0-4.0/257.0)*sel-1.0+2.0/257.0;\n"
    "    gl_Position.y = 0.0;\n"
    "    gl_Position.z = 0.0;\n"
    "}\n"
;
const char *histogramRendering_frag =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect HistogramTex;\n"
    "uniform sampler2DRect MaximumRedTex;\n"
    "uniform sampler2DRect MaximumGreenTex;\n"
    "uniform sampler2DRect MaximumBlueTex;\n"
    "uniform int channel;\n"
    "void main()\n"
    "{\n"
    "	if(channel == 0){\n"
    "		gl_FragColor =vec4(0.8,0.8,0.8,0.8);\n"
    "	}else if(channel == 1){\n"
    "		gl_FragColor =vec4(1.0,0.0,0.0,0.8);\n"
    "	}else if(channel == 2){\n"
    "		gl_FragColor =vec4(0.0,1.0,0.0,0.8);\n"
    "	}else if(channel == 3){\n"
    "		gl_FragColor =vec4(0.0,0.0,1.0,0.8);\n"
    "	}\n"
    "   \n"
    "}\n"
;
const char *histogramRenderingVertex_vert =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect HistogramTex;\n"
    "uniform sampler2DRect MaximumRedTex;\n"
    "uniform sampler2DRect MaximumGreenTex;\n"
    "uniform sampler2DRect MaximumBlueTex;\n"
    "uniform int channel;\n"
    "attribute vec3 TexCoord;\n"
    "void main()\n"
    "{\n"
    "\n"
    "    vec4 c = texture2DRect(HistogramTex, TexCoord.xy  );\n"
    "	float bottom = TexCoord.z;\n"
    "	float maximum = 0.0;\n"
    "	float maximumRed = texture2DRect(MaximumRedTex,vec2(0.0,0.0)).r;\n"
    "	float maximumGreen = texture2DRect(MaximumGreenTex,vec2(0.0,0.0)).r;\n"
    "	float maximumBlue = texture2DRect(MaximumBlueTex,vec2(0.0,0.0)).r;\n"
    "	maximum = max(max(maximumRed,maximumGreen),maximumBlue);\n"
    "	if(maximum == 0.0){\n"
    "		maximum = 1000000.0;\n"
    "	}\n"
    "// set new point position to the color intensity in [-1.0,1.0] interval\n"
    "// as this is homogeneous coord. clip space\n"
    "    gl_Position.x =(2.0-4.0/257.0)*(TexCoord.x/255.0)-1.0+2.0/257.0;\n"
    "	if(bottom == 1.0){\n"
    "           gl_Position.y = -1.0;\n"
    "	}else{\n"
    "		float y = c.r/maximum;\n"
    "		gl_Position.y = 2.0*y-1.0;\n"
    "	}\n"
    "    gl_Position.z = 0.0;\n"
    "}\n"
;
const char* minimal_vert =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "gl_TexCoord[0]=gl_MultiTexCoord0;"
    "gl_Position = ftransform();"
;
const char *histogramMaximum_frag =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect Tex;\n"
    "void main()\n"
    "{\n"
    "    vec4 a,b,c,d;\n"
    "	vec2 texCoord = gl_TexCoord[0].st;\n"
    "	texCoord.s = (texCoord.s-0.5) * 4.0 + 0.5;\n"
    "    a = texture2DRect(Tex,texCoord.st);\n"
    "	vec2 texCoord1,texCoord2,texCoord3;\n"
    "	texCoord1 = texCoord.st + vec2(1.0,0.0);\n"
    "	texCoord2 = texCoord.st + vec2(2.0,0.0);\n"
    "	texCoord3 = texCoord.st + vec2(3.0,0.0);\n"
    "	if(texCoord1.s <= 256.0){\n"
    "		b = texture2DRect(Tex,texCoord1);\n"
    "	}else{\n"
    "		b = a;\n"
    "	}\n"
    "	if(texCoord2.s <= 256.0){\n"
    "		c = texture2DRect(Tex,texCoord2);\n"
    "	}else{\n"
    "		c = a;\n"
    "	}\n"
    "	if(texCoord3.s <= 256.0){\n"
    "		d = texture2DRect(Tex,texCoord3);\n"
    "	}else{\n"
    "		d = a;\n"
    "	}\n"
    "    gl_FragColor = vec4(max(max(a.r,b.r),max(c.r,d.r)),0.0,0.0,1.0);\n"
    "}\n"
;

NATRON_NAMESPACE_EXIT
