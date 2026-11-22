#!/usr/bin/env python3
"""Generates hand-assembled SPIR-V vertex shaders (same logic as clip4rsx.vert: pos = (p, 0, 1); clip = (0.5, d, 0.5, 0.5)) that differ ONLY in how
Position / ClipDistance are declared, to isolate which declaration form the Adreno 710 mishandles.
  block      : both are members of a gl_PerVertex Block (what GLSL emits)                        [works]
  clip_solo  : Position in a Block, ClipDistance a standalone Output variable decorated BuiltIn
  pos_solo   : ClipDistance alone in a Block, Position a standalone Output variable
  both_solo  : both standalone variables (what the HLSL front end emits), direct stores
  both_solo_arr : like both_solo but the whole float[4] is stored at once (exercises the rewrite of a whole-array OpStore)
usage: gen_spvasm.py <mode> > file.spvasm"""
import sys
mode = sys.argv[1]
arrstore = mode == 'both_solo_arr'
if arrstore: mode = 'both_solo'
pos_in_block  = mode in ('block', 'clip_solo')
clip_in_block = mode in ('block', 'pos_solo')
members = []
if pos_in_block:  members.append('pos')
if clip_in_block: members.append('clip')
out = []
w = out.append
w('OpCapability Shader'); w('OpCapability ClipDistance'); w('%1 = OpExtInstImport "GLSL.std.450"'); w('OpMemoryModel Logical GLSL450')
ifaces = []
if members: ifaces.append('%blk')
if not pos_in_block: ifaces.append('%posv')
if not clip_in_block: ifaces.append('%clipv')
w('OpEntryPoint Vertex %main "main" ' + ' '.join(ifaces) + ' %pin %din')
for i, m in enumerate(members):
    w(f"OpMemberDecorate %PerVertex {i} BuiltIn {'Position' if m == 'pos' else 'ClipDistance'}")
if members: w('OpDecorate %PerVertex Block')
if not pos_in_block: w('OpDecorate %posv BuiltIn Position')
if not clip_in_block: w('OpDecorate %clipv BuiltIn ClipDistance')
w('OpDecorate %pin Location 0'); w('OpDecorate %din Location 1')
w('%void = OpTypeVoid'); w('%fn = OpTypeFunction %void'); w('%float = OpTypeFloat 32'); w('%v2 = OpTypeVector %float 2'); w('%v4 = OpTypeVector %float 4')
w('%uint = OpTypeInt 32 0'); w('%int = OpTypeInt 32 1'); w('%uint_4 = OpConstant %uint 4'); w('%arr4 = OpTypeArray %float %uint_4')
if members:
    w('%PerVertex = OpTypeStruct ' + ' '.join('%v4' if m == 'pos' else '%arr4' for m in members))
    w('%pblk = OpTypePointer Output %PerVertex'); w('%blk = OpVariable %pblk Output')
w('%pov4 = OpTypePointer Output %v4'); w('%poarr = OpTypePointer Output %arr4'); w('%pof = OpTypePointer Output %float')
if not pos_in_block:  w('%posv = OpVariable %pov4 Output')
if not clip_in_block: w('%clipv = OpVariable %poarr Output')
w('%piv2 = OpTypePointer Input %v2'); w('%pif = OpTypePointer Input %float'); w('%pin = OpVariable %piv2 Input'); w('%din = OpVariable %pif Input')
for k in range(4): w(f'%int_{k} = OpConstant %int {k}')
w('%f0 = OpConstant %float 0'); w('%f1 = OpConstant %float 1'); w('%fh = OpConstant %float 0.5')
w('%main = OpFunction %void None %fn'); w('%entry = OpLabel')
w('%p = OpLoad %v2 %pin'); w('%x = OpCompositeExtract %float %p 0'); w('%y = OpCompositeExtract %float %p 1'); w('%pos4 = OpCompositeConstruct %v4 %x %y %f0 %f1'); w('%d = OpLoad %float %din')
if pos_in_block: w('%pp = OpAccessChain %pov4 %blk %int_0')
w('OpStore %pp %pos4' if pos_in_block else 'OpStore %posv %pos4')
vals = ['%fh', '%d', '%fh', '%fh']
if arrstore:
    w('%arrv = OpCompositeConstruct %arr4 ' + ' '.join(vals)); w('OpStore %clipv %arrv'); w('OpReturn'); w('OpFunctionEnd'); print('\n'.join(out)); sys.exit(0)
for k in range(4):
    if clip_in_block:
        idx = members.index('clip')
        w(f'%c{k} = OpAccessChain %pof %blk %int_{idx} %int_{k}')
    else:
        w(f'%c{k} = OpAccessChain %pof %clipv %int_{k}')
    w(f'OpStore %c{k} {vals[k]}')
w('OpReturn'); w('OpFunctionEnd')
print('\n'.join(out))
