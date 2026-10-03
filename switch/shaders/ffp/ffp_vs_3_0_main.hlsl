// Émulation du pipeline fixe de Direct3D 9 (dessins faits sans programme : interface CEGUI...), sommets.
// Rempli par switch/tools/rendu_hors_ligne : tl_wvp = projection * vue * monde (Matrix4 d'Ogre, convention
// vecteur colonne), rangé par lignes.
row_major float4x4 tl_wvp;

void main(
	[[vk::location(0)]] in float4 iPos : POSITION,
	[[vk::location(4)]] in float4 iColor : COLOR0,
	[[vk::location(6)]] in float4 iUv0 : TEXCOORD0,
	[[vk::location(7)]] in float4 iUv1 : TEXCOORD1,
	out float4 oPos : SV_Position,
	[[vk::location(8)]] out float4 oColor : COLOR0,
	[[vk::location(0)]] out float4 oUv0 : TEXCOORD0,
	[[vk::location(1)]] out float4 oUv1 : TEXCOORD1)
{
	oPos = mul(tl_wvp, float4(iPos.xyz, 1.0));
	oColor = iColor;
	oUv0 = iUv0;
	oUv1 = iUv1;
}
