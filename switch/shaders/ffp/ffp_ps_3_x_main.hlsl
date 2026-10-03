// Émulation du pipeline fixe de Direct3D 9, pixels : deux étages de combinaison de textures d'Ogre
// (LayerBlendModeEx). Pour l'étage i :
//   tl_colour_op[i] / tl_alpha_op[i] = (opération LBX, source1 LBS, source2 LBS, actif)
//   tl_colour_arg1[i], tl_colour_arg2[i] = couleurs manuelles ; tl_alpha_arg[i] = (alphaArg1, alphaArg2, factor)
// Sources : 0 courant, 1 texture, 2 diffuse (couleur du sommet), 3 spéculaire, 4 manuelle.
sampler2D tl_tex0 : register(s0);
sampler2D tl_tex1 : register(s1);
float4 tl_colour_op[2];
float4 tl_alpha_op[2];
float4 tl_colour_arg1[2];
float4 tl_colour_arg2[2];
float4 tl_alpha_arg[2];
float4 torchlight_alpha_test;

float4 Source(int s, float4 current, float4 texel, float4 diffuse, float4 manual)
{
	if (s == 0) return current;
	if (s == 1) return texel;
	if (s == 2) return diffuse;
	if (s == 3) return float4(0, 0, 0, 0);
	return manual;
}

float4 Combine(int op, float4 a, float4 b, float4 current, float4 texel, float4 diffuse, float factor)
{
	if (op == 0) return a;                              // LBX_SOURCE1
	if (op == 1) return b;                              // LBX_SOURCE2
	if (op == 2) return a * b;                          // LBX_MODULATE
	if (op == 3) return a * b * 2.0;                    // LBX_MODULATE_X2
	if (op == 4) return a * b * 4.0;                    // LBX_MODULATE_X4
	if (op == 5) return a + b;                          // LBX_ADD
	if (op == 6) return a + b - 0.5;                    // LBX_ADD_SIGNED
	if (op == 7) return a + b - a * b;                  // LBX_ADD_SMOOTH
	if (op == 8) return a - b;                          // LBX_SUBTRACT
	if (op == 9) return lerp(b, a, diffuse.a);          // LBX_BLEND_DIFFUSE_ALPHA
	if (op == 10) return lerp(b, a, texel.a);           // LBX_BLEND_TEXTURE_ALPHA
	if (op == 11) return lerp(b, a, current.a);         // LBX_BLEND_CURRENT_ALPHA
	if (op == 12) return lerp(b, a, factor);            // LBX_BLEND_MANUAL
	if (op == 13) return dot(a.rgb - 0.5, b.rgb - 0.5) * 4.0; // LBX_DOTPRODUCT
	return lerp(b, a, diffuse);                         // LBX_BLEND_DIFFUSE_COLOUR
}

void main(
	[[vk::location(8)]] in float4 iColor : COLOR0,
	[[vk::location(0)]] in float4 iUv0 : TEXCOORD0,
	[[vk::location(1)]] in float4 iUv1 : TEXCOORD1,
	out float4 oColor : SV_Target0)
{
	float4 current = iColor;
	for (int i = 0; i < 2; i++)
	{
		if (tl_colour_op[i].w < 0.5) break;             // étage désactivé : fin de la cascade
		const float4 texel = i == 0 ? tex2D(tl_tex0, iUv0.xy) : tex2D(tl_tex1, iUv1.xy);
		const float4 ca = Source((int)tl_colour_op[i].y, current, texel, iColor, tl_colour_arg1[i]);
		const float4 cb = Source((int)tl_colour_op[i].z, current, texel, iColor, tl_colour_arg2[i]);
		const float4 aa = Source((int)tl_alpha_op[i].y, current, texel, iColor, tl_alpha_arg[i].xxxx);
		const float4 ab = Source((int)tl_alpha_op[i].z, current, texel, iColor, tl_alpha_arg[i].yyyy);
		const float3 rgb = Combine((int)tl_colour_op[i].x, ca, cb, current, texel, iColor, tl_alpha_arg[i].z).rgb;
		const float a = Combine((int)tl_alpha_op[i].x, aa, ab, current, texel, iColor, tl_alpha_arg[i].z).a;
		current = saturate(float4(rgb, a));
	}
	oColor = current;
	{
		const float tl_a = oColor.a;
		const float tl_r = torchlight_alpha_test.x;
		const int tl_f = (int)torchlight_alpha_test.y;
		bool tl_pass = true;
		if (tl_f == 0) tl_pass = false;
		else if (tl_f == 2) tl_pass = tl_a < tl_r;
		else if (tl_f == 3) tl_pass = tl_a <= tl_r;
		else if (tl_f == 4) tl_pass = tl_a == tl_r;
		else if (tl_f == 5) tl_pass = tl_a != tl_r;
		else if (tl_f == 6) tl_pass = tl_a >= tl_r;
		else if (tl_f == 7) tl_pass = tl_a > tl_r;
		if (!tl_pass) discard;
	}
}
