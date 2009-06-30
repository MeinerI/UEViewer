#include "Core.h"

#include "UnrealClasses.h"
#include "MeshInstance.h"

#include "GlWindow.h"


// debugging
//#define CHECK_INFLUENCES		1
//#define SHOW_INFLUENCES		1
//#define SHOW_ANIM				1


struct CMeshBoneData
{
	// static data (computed after mesh loading)
	int			BoneMap;			// index of bone in animation tracks
	CCoords		RefCoords;			// coordinates of bone in reference pose (unused!!)
	CCoords		RefCoordsInv;		// inverse of RefCoords
	int			SubtreeSize;		// count of all children bones (0 for leaf bone)
	// dynamic data
	// skeleton configuration
	float		Scale;				// bone scale; 1=unscaled
	int			FirstChannel;		// first animation channel, affecting this bone
	// current pose
	CCoords		Coords;				// current coordinates of bone, model-space
	CCoords		Transform;			// used to transform vertex from reference pose to current pose
	// data for tweening; bone-space
	CVec3		Pos;				// current position of bone
	CQuat		Quat;				// current orientation quaternion
};


#define ANIM_UNASSIGNED			-2
#define MAX_MESHBONES			512


/*-----------------------------------------------------------------------------
	Create/destroy
-----------------------------------------------------------------------------*/

CSkelMeshInstance::~CSkelMeshInstance()
{
	if (BoneData)
	{
		delete BoneData;
		delete MeshVerts;
		delete MeshNormals;
		delete RefNormals;
	}
	if (InfColors) delete InfColors;
}


void CSkelMeshInstance::ClearSkelAnims()
{
	// init 1st animation channel with default pose
	for (int i = 0; i < MAX_SKELANIMCHANNELS; i++)
	{
		Channels[i].AnimIndex1     = ANIM_UNASSIGNED;
		Channels[i].AnimIndex2     = ANIM_UNASSIGNED;
		Channels[i].SecondaryBlend = 0;
		Channels[i].BlendAlpha     = 1;
		Channels[i].RootBone       = 0;
	}
}


/* Iterate bone (sub)tree and do following:
 *	- find all direct childs of bone 'Index', check sort order; bones should be
 *	  sorted in hierarchy order (1st child and its children first, other childs next)
 *	  Example of tree:
 *		Bone1
 *		+- Bone11
 *		|  +- Bone111
 *		+- Bone12
 *		|  +- Bone121
 *		|  +- Bone122
 *		+- Bone13
 *	  Sorted order: Bone1, Bone11, Bone111, Bone12, Bone121, Bone122, Bone13
 *	  Note: it seems, Unreal already have sorted bone list (assumed in other code,
 *	  verified by 'assert(currIndex == Index)')
 *	- compute subtree sizes ('Sizes' array)
 *	- compute bone hierarchy depth ('Depth' array, for debugging only)
 */
static int CheckBoneTree(const TArray<FMeshBone> &Bones, int Index,
	int *Sizes, int *Depth, int &numIndices, int maxIndices)
{
	assert(numIndices < maxIndices);
	static int depth = 0;
	// remember curerent index, increment for recustion
	int currIndex = numIndices++;
	// find children of Bone[Index]
	int treeSize = 0;
	for (int i = Index + 1; i < Bones.Num(); i++)
		if (Bones[i].ParentIndex == Index)
		{
			// recurse
			depth++;
			treeSize += CheckBoneTree(Bones, i, Sizes, Depth, numIndices, maxIndices);
			depth--;
		}
	// store gathered information
//	assert(currIndex == Index);		//??
	if (currIndex != Index) appNotify("Strange skeleton, check childs of bone %d", Index);
	Sizes[currIndex] = treeSize;
	Depth[currIndex] = depth;
	return treeSize + 1;
}


static void BuildNormals(const USkeletalMesh *Mesh, CVec3 *Normals)
{
	int i;
	for (i = 0; i < Mesh->Triangles.Num(); i++)
	{
		const VTriangle &Face = Mesh->Triangles[i];
		// get vertex indices
		int i1 = Mesh->Wedges[Face.WedgeIndex[0]].iVertex;
		int i2 = Mesh->Wedges[Face.WedgeIndex[1]].iVertex;
		int i3 = Mesh->Wedges[Face.WedgeIndex[2]].iVertex;
		// compute edges
		const CVec3 &V1 = (CVec3&)Mesh->Points[i1];
		const CVec3 &V2 = (CVec3&)Mesh->Points[i2];
		const CVec3 &V3 = (CVec3&)Mesh->Points[i3];
		CVec3 D1, D2, D3;
		VectorSubtract(V2, V1, D1);
		VectorSubtract(V3, V2, D2);
		VectorSubtract(V1, V3, D3);
		// compute normal
		CVec3 norm;
		cross(D2, D1, norm);
		norm.Normalize();
		// compute angles
		D1.Normalize();
		D2.Normalize();
		D3.Normalize();
		float angle1 = acos(-dot(D1, D3));
		float angle2 = acos(-dot(D1, D2));
		float angle3 = acos(-dot(D2, D3));
		// add normals for triangle verts
		VectorMA(Normals[i1], angle1, norm);
		VectorMA(Normals[i2], angle2, norm);
		VectorMA(Normals[i3], angle3, norm);
	}
	// normalize normals
	for (i = 0; i < Mesh->Points.Num(); i++)
		Normals[i].Normalize();
}


void CSkelMeshInstance::SetMesh(const ULodMesh *LodMesh)
{
	guard(CSkelMeshInstance::SetMesh);

	CLodMeshInstance::SetMesh(LodMesh);
	const USkeletalMesh *Mesh = static_cast<const USkeletalMesh*>(LodMesh);

	int NumBones = Mesh->RefSkeleton.Num();
	int NumVerts = Mesh->Points.Num();
	const UMeshAnimation *Anim = Mesh->Animation;

	// allocate some arrays
	if (BoneData)
	{
		delete BoneData;
		delete MeshVerts;
		delete MeshNormals;
		delete RefNormals;
	}
	if (InfColors)
	{
		delete InfColors;
		InfColors = NULL;
	}
	BoneData    = new CMeshBoneData[NumBones];
	MeshVerts   = new CVec3        [NumVerts];
	MeshNormals = new CVec3        [NumVerts];
	RefNormals  = new CVec3        [NumVerts];

	int i;
	CMeshBoneData *data;
	for (i = 0, data = BoneData; i < NumBones; i++, data++)
	{
		const FMeshBone &B = Mesh->RefSkeleton[i];
		// NOTE: assumed, that parent bones goes first
		assert(B.ParentIndex <= i);

		// find reference bone in animation track
		data->BoneMap = INDEX_NONE;
		if (Anim)
		{
			for (int j = 0; j < Anim->RefBones.Num(); j++)
				if (!stricmp(B.Name, Anim->RefBones[j].Name))
				{
					data->BoneMap = j;
					break;
				}
		}

		// compute reference bone coords
		CVec3 BP;
		CQuat BO;
		// get default pose
		BP = (CVec3&)B.BonePos.Position;
		BO = (CQuat&)B.BonePos.Orientation;
		if (!i) BO.Conjugate();

		CCoords &BC = data->RefCoords;
		BC.origin = BP;
		BO.ToAxis(BC.axis);
		// move bone position to global coordinate space
		if (i)	// do not rotate root bone
			BoneData[B.ParentIndex].RefCoords.UnTransformCoords(BC, BC);
		// store inverted transformation too
		InvertCoords(data->RefCoords, data->RefCoordsInv);
#if 0
	//!!
if (i == 32 || i == 34)
{
	appNotify("Bone %d (%8.3f %8.3f %8.3f) - (%8.3f %8.3f %8.3f %8.3f)", i, VECTOR_ARG(BP), QUAT_ARG(BO));
#define C data->RefCoords
	appNotify("REF   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	appNotify("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	appNotify("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	appNotify("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->RefCoordsInv
	appNotify("REFIN : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	appNotify("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	appNotify("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	appNotify("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
}
#endif
		// initialize skeleton configuration
		data->Scale = 1.0f;			// default bone scale
	}

	BuildNormals(Mesh, RefNormals);

	// normalize VertInfluences: sum of all influences may be != 1
	// (possible situation, see SkaarjAnims/Skaarj2, SkaarjAnims/Skaarj_Skel, XanRobots/XanF02)
	//!! should be done in USkeletalMesh
	float *VertSumWeights = new float[NumVerts];	// zeroed
	int   *VertInfCount   = new int  [NumVerts];	// zeroed
	// count sum of weights for all verts
	for (i = 0; i < Mesh->VertInfluences.Num(); i++)
	{
		const FVertInfluences &Inf = Mesh->VertInfluences[i];
		int PointIndex = Inf.PointIndex;
		assert(PointIndex < NumVerts);
		VertSumWeights[PointIndex] += Inf.Weight;
		VertInfCount  [PointIndex]++;
	}
#if 0
	// notify about wrong weights
	for (i = 0; i < NumVerts; i++)
	{
		if (fabs(VertSumWeights[i] - 1.0f) < 0.01f) continue;
		appNotify("Vert[%d] weight sum=%g (%d weights)", i, VertSumWeights[i], VertInfCount[i]);
	}
#endif
	// normalize weights
	for (i = 0; i < Mesh->VertInfluences.Num(); i++)
	{
		FVertInfluences &Inf = const_cast<USkeletalMesh*>(Mesh)->VertInfluences[i]; // to avoid const_cast, implement in mesh
		int PointIndex = Inf.PointIndex;
		float sum = VertSumWeights[PointIndex];
		if (fabs(sum - 1.0f) < 0.01f) continue;
		assert(sum > 0.01f);	// no division by zero
		Inf.Weight = Inf.Weight / sum;
	}

	delete VertSumWeights;
	delete VertInfCount;

	// check bones tree
	if (NumBones)
	{
		//!! should be done in USkeletalMesh
		guard(VerifySkeleton);
		int treeSizes[MAX_MESHBONES], depth[MAX_MESHBONES];
		int numIndices = 0;
		CheckBoneTree(Mesh->RefSkeleton, 0, treeSizes, depth, numIndices, MAX_MESHBONES);
		assert(numIndices == NumBones);
		for (i = 0; i < numIndices; i++)
			BoneData[i].SubtreeSize = treeSizes[i];	// remember subtree size
		unguard;
	}

	ClearSkelAnims();
	PlayAnim(NULL);

	unguard;
}


void CSkelMeshInstance::DumpBones()
{
#if 1
	const USkeletalMesh *Mesh = static_cast<const USkeletalMesh*>(pMesh);
	int treeSizes[MAX_MESHBONES], depth[MAX_MESHBONES];
	int numIndices = 0;
	CheckBoneTree(Mesh->RefSkeleton, 0, treeSizes, depth, numIndices, MAX_MESHBONES);
	//?? dump tree; separate function (requires depth information)
	for (int i = 0; i < numIndices; i++)
	{
		const FMeshBone &B = Mesh->RefSkeleton[i];
		int parent = B.ParentIndex;
		printf("bone#%2d (parent %2d); tree size: %2d   ", i, parent, treeSizes[i]);
#if 1
		for (int j = 0; j < depth[i]; j++)
		{
	#if 0
			// simple picture
			printf("|  ");
	#else
			// graph-like picture
			bool found = false;
			for (int n = i+1; n < numIndices; n++)
			{
				if (depth[n] >  j+1) continue;
				if (depth[n] == j+1) found = true;
				break;
			}
		#if _WIN32
			// pseudographics
			if (j == depth[i]-1)
				printf(found ? "\xC3\xC4\xC4" : "\xC0\xC4\xC4");	// [+--] : [\--]
			else
                printf(found ? "\xB3  " : "   ");					// [|  ] : [   ]
		#else
			// ASCII
			if (j == depth[i]-1)
				printf(found ? "+--" : "\\--");
			else
				printf(found ? "|  " : "   ");
        #endif
	#endif
		}
		printf("%s\n", *B.Name);
#else
		printf("%s {%g %g %g} {%g %g %g %g}\n", *B.Name, FVECTOR_ARG(B.BonePos.Position), FQUAT_ARG(B.BonePos.Orientation));
#endif
	}
#endif
}


/*-----------------------------------------------------------------------------
	Miscellaneous
-----------------------------------------------------------------------------*/

int CSkelMeshInstance::FindBone(const char *BoneName) const
{
	const USkeletalMesh *Mesh = GetMesh();
	for (int i = 0; i < Mesh->RefSkeleton.Num(); i++)
		if (!strcmp(Mesh->RefSkeleton[i].Name, BoneName))
			return i;
	return INDEX_NONE;
}


int CSkelMeshInstance::FindAnim(const char *AnimName) const
{
	const USkeletalMesh *Mesh  = GetMesh();
	const UMeshAnimation *Anim = Mesh->Animation;
	if (!Anim || !AnimName)
		return INDEX_NONE;
	for (int i = 0; i < Anim->AnimSeqs.Num(); i++)
		if (!strcmp(Anim->AnimSeqs[i].Name, AnimName))
			return i;
	return INDEX_NONE;
}


void CSkelMeshInstance::SetBoneScale(const char *BoneName, float scale)
{
	int BoneIndex = FindBone(BoneName);
	if (BoneIndex == INDEX_NONE) return;
	BoneData[BoneIndex].Scale = scale;
}


/*-----------------------------------------------------------------------------
	Skeletal animation itself
-----------------------------------------------------------------------------*/

#define MAX_LINEAR_KEYS		4

static int FindTimeKey(const TArray<float> &KeyTime, float Frame)
{
	guard(FindTimeKey);

	// find index in time key array
	int NumKeys = KeyTime.Num();
	// *** binary search ***
	int Low = 0, High = NumKeys-1;
	while (Low + MAX_LINEAR_KEYS < High)
	{
		int Mid = (Low + High) / 2;
		if (Frame < KeyTime[Mid])
			High = Mid-1;
		else
			Low = Mid;
	}
	// *** linear search ***
	int i;
	for (i = Low; i <= High; i++)
	{
		float CurrKeyTime = KeyTime[i];
		if (Frame == CurrKeyTime)
			return i;		// exact key
		if (Frame < CurrKeyTime)
			return (i > 0) ? i - 1 : 0;	// previous key
	}
	if (i > High)
		i = High;
	return i;

	unguard;
}


// In:  KeyTime, Frame, NumFrames, Loop
// Out: X - previous key index, Y - next key index, F - fraction between keys
static void GetKeyParams(const TArray<float> &KeyTime, float Frame, float NumFrames, bool Loop, int &X, int &Y, float &F)
{
	guard(GetKeyParams);
	X = FindTimeKey(KeyTime, Frame);
	Y = X + 1;
	int NumTimeKeys = KeyTime.Num();
	if (Y >= NumTimeKeys)
	{
		if (!Loop)
		{
			// clamp animation
			Y = NumTimeKeys - 1;
			assert(X == Y);
			F = 0;
		}
		else
		{
			// loop animation
			Y = 0;
			F = (Frame - KeyTime[X]) / (NumFrames - KeyTime[X]);
		}
	}
	else
	{
		F = (Frame - KeyTime[X]) / (KeyTime[Y] - KeyTime[X]);
	}
	unguard;
}


// not 'static', because used in ExportPsa()
//?? place function(s) into UMeshAnimation
void GetBonePosition(const AnalogTrack &A, float Frame, float NumFrames, bool Loop,
	CVec3 &DstPos, CQuat &DstQuat)
{
	guard(GetBonePosition);

	// fast case: 1 frame only
	if (A.KeyTime.Num() == 1 || NumFrames == 1 || Frame == 0)
	{
		DstPos  = (CVec3&)A.KeyPos[0];
		DstQuat = (CQuat&)A.KeyQuat[0];
		return;
	}

	// data for lerping
	int posX, rotX;			// index of previous frame
	int posY, rotY;			// index of next frame
	float posF, rotF;		// fraction between X and Y for lerping

	int NumTimeKeys = A.KeyTime.Num();
	int NumPosKeys  = A.KeyPos.Num();
	int NumRotKeys  = A.KeyQuat.Num();

	if (NumTimeKeys)
	{
		// here: KeyPos and KeyQuat sizes either equals to 1 or equals to KeyTime size
		assert(NumPosKeys == 1 || NumPosKeys == NumTimeKeys);
		assert(NumRotKeys == 1 || NumRotKeys == NumTimeKeys);

		GetKeyParams(A.KeyTime, Frame, NumFrames, Loop, posX, posY, posF);
		rotX = posX;
		rotY = posY;
		rotF = posF;

		if (NumPosKeys == 1)
		{
			posX = posY = 0;
			posF = 0;
		}
		if (NumRotKeys == 1)
		{
			rotX = rotY = 0;
			rotF = 0;
		}
	}
	else
	{
		// empty KeyTime array - keys are evenly spaced on a time line
		// note: KeyPos and KeyQuat sizes can be different
#if UNREAL3
		if (A.KeyPosTime.Num())
		{
			GetKeyParams(A.KeyPosTime, Frame, NumFrames, Loop, posX, posY, posF);
		}
		else
#endif
		if (NumPosKeys > 1)
		{
			float Position = Frame / NumFrames * NumPosKeys;
			posX = appFloor(Position);
			posF = Position - posX;
			posY = posX + 1;
			if (posY >= NumPosKeys)
			{
				if (!Loop)
				{
					posY = NumPosKeys - 1;
					posF = 0;
				}
				else
					posY = 0;
			}
		}
		else
		{
			posX = posY = 0;
			posF = 0;
		}

#if UNREAL3
		if (A.KeyQuatTime.Num())
		{
			GetKeyParams(A.KeyQuatTime, Frame, NumFrames, Loop, rotX, rotY, rotF);
		}
		else
#endif
		if (NumRotKeys > 1)
		{
			float Position = Frame / NumFrames * NumRotKeys;
			rotX = appFloor(Position);
			rotF = Position - rotX;
			rotY = rotX + 1;
			if (rotY >= NumRotKeys)
			{
				if (!Loop)
				{
					rotY = NumRotKeys - 1;
					rotF = 0;
				}
				else
					rotY = 0;
			}
		}
		else
		{
			rotX = rotY = 0;
			rotF = 0;
		}
	}

	// get position
	if (posF > 0)
		Lerp((CVec3&)A.KeyPos[posX], (CVec3&)A.KeyPos[posY], posF, DstPos);
	else
		DstPos = (CVec3&)A.KeyPos[posX];
	// get orientation
	if (rotF > 0)
		Slerp((CQuat&)A.KeyQuat[rotX], (CQuat&)A.KeyQuat[rotY], rotF, DstQuat);
	else
		DstQuat = (CQuat&)A.KeyQuat[rotX];

	unguard;
}


static int BoneUpdateCounts[MAX_MESHBONES];	//!! remove later

void CSkelMeshInstance::UpdateSkeleton()
{
	guard(CSkelMeshInstance::UpdateSkeleton);

	const USkeletalMesh  *Mesh = GetMesh();
	const UMeshAnimation *Anim = Mesh->Animation;

	// process all animation channels
	assert(MaxAnimChannel < MAX_SKELANIMCHANNELS);
	int Stage;
	CAnimChan *Chn;
	memset(BoneUpdateCounts, 0, sizeof(BoneUpdateCounts)); //!! remove later
	for (Stage = 0, Chn = Channels; Stage <= MaxAnimChannel; Stage++, Chn++)
	{
		if (Stage > 0 && (Chn->AnimIndex1 == ANIM_UNASSIGNED || Chn->BlendAlpha <= 0))
			continue;

		const MotionChunk  *Motion1  = NULL, *Motion2  = NULL;
		const FMeshAnimSeq *AnimSeq1 = NULL, *AnimSeq2 = NULL;
		float Time2;
		if (Chn->AnimIndex1 >= 0)		// not INDEX_NONE or ANIM_UNASSIGNED
		{
			Motion1  = &Anim->Moves   [Chn->AnimIndex1];
			AnimSeq1 = &Anim->AnimSeqs[Chn->AnimIndex1];
			if (Chn->AnimIndex2 >= 0 && Chn->SecondaryBlend)
			{
				Motion2  = &Anim->Moves   [Chn->AnimIndex2];
				AnimSeq2 = &Anim->AnimSeqs[Chn->AnimIndex2];
				// compute time for secondary channel; always in sync with primary channel
				Time2 = Chn->Time / AnimSeq1->NumFrames * AnimSeq2->NumFrames;
			}
		}

		// compute bone range, affected by specified animation bone
		int firstBone = Chn->RootBone;
		int lastBone  = firstBone + BoneData[firstBone].SubtreeSize;
		assert(lastBone < Mesh->RefSkeleton.Num());

		int i;
		CMeshBoneData *data;
		for (i = firstBone, data = BoneData + firstBone; i <= lastBone; i++, data++)
		{
			if (Stage < data->FirstChannel)
			{
				// this bone position will be overrided in following channel(s); all
				// subhierarchy bones should be overrided too; skip whole subtree
				int skip = data->SubtreeSize;
				// note: 'skip' equals to subtree size; current bone is excluded - it
				// will be skipped by 'for' operator (after 'continue')
				i    += skip;
				data += skip;
				continue;
			}

			CVec3 BP;
			CQuat BO;
			int BoneIndex = data->BoneMap;
			// check for disabled bone (required for Tribes3)
			if (Motion1 && Motion1->BoneIndices.Num() && BoneIndex != INDEX_NONE &&
				Motion1->BoneIndices[BoneIndex] == INDEX_NONE)
				BoneIndex = INDEX_NONE;		// will use RefSkeleton for this bone
			// compute bone orientation
			if (Motion1 && BoneIndex != INDEX_NONE)
			{
				// get bone position from track
				if (!Motion2 || Chn->SecondaryBlend != 1.0f)
				{
					BoneUpdateCounts[i]++;		//!! remove later
					GetBonePosition(Motion1->AnimTracks[BoneIndex], Chn->Time, AnimSeq1->NumFrames,
						Chn->Looped, BP, BO);
//const char *bname = *Mesh->RefSkeleton[i].Name;
//CQuat BOO = BO;
//if (!strcmp(bname, "b_MF_UpperArm_L")) { BO.Set(-0.225, -0.387, -0.310,  0.839); }
#if SHOW_ANIM
if (i == 6 || i == 8 || i == 10 || i == 11 || i == 29)	//??
					DrawTextLeft("Bone (%s) : P{ %8.3f %8.3f %8.3f }  Q{ %6.3f %6.3f %6.3f %6.3f }",
						*Mesh->RefSkeleton[i].Name, VECTOR_ARG(BP), QUAT_ARG(BO));
//if (!strcmp(bname, "b_MF_UpperArm_L")) DrawTextLeft("%g %g %g %g [%g %g]", BO.x-BOO.x,BO.y-BOO.y,BO.z-BOO.z,BO.w-BOO.w, BO.w, BOO.w);
#endif
//BO.Normalize();
				}
				// blend secondary animation
				if (Motion2)
				{
					CVec3 BP2;
					CQuat BO2;
					BoneUpdateCounts[i]++;		//!! remove later
					GetBonePosition(Motion2->AnimTracks[BoneIndex], Time2, AnimSeq2->NumFrames,
						Chn->Looped, BP2, BO2);
					if (Chn->SecondaryBlend == 1.0f)
					{
						BO = BO2;
						BP = BP2;
					}
					else
					{
						Lerp (BP, BP2, Chn->SecondaryBlend, BP);
						Slerp(BO, BO2, Chn->SecondaryBlend, BO);
					}
				}
			}
			else
			{
				// get default bone position
				const FMeshBone &B = Mesh->RefSkeleton[i];
				BP = (CVec3&)B.BonePos.Position;
				BO = (CQuat&)B.BonePos.Orientation;
			}
			if (!i) BO.Conjugate();

			// tweening
			if (Chn->TweenTime > 0)
			{
				// interpolate orientation using AnimTweenStep
				// current orientation -> {BP,BO}
				Lerp (data->Pos,  BP, Chn->TweenStep, BP);
				Slerp(data->Quat, BO, Chn->TweenStep, BO);
			}
			// blending with previous channels
			if (Chn->BlendAlpha < 1.0f)
			{
				Lerp (data->Pos,  BP, Chn->BlendAlpha, BP);
				Slerp(data->Quat, BO, Chn->BlendAlpha, BO);
			}

			data->Quat = BO;
			data->Pos  = BP;
		}
	}

	// transform bones using skeleton hierarchy
	int i;
	CMeshBoneData *data;
	for (i = 0, data = BoneData; i < Mesh->RefSkeleton.Num(); i++, data++)
	{
		CCoords &BC = data->Coords;
		BC.origin = data->Pos;
		data->Quat.ToAxis(BC.axis);

		// move bone position to global coordinate space
		if (!i)
		{
			// root bone - use BaseTransform
			// can use inverted BaseTransformScaled to avoid 'slow' operation
			BaseTransformScaled.TransformCoordsSlow(BC, BC);
		}
		else
		{
			// other bones - rotate around parent bone
			BoneData[Mesh->RefSkeleton[i].ParentIndex].Coords.UnTransformCoords(BC, BC);
		}
		// deform skeleton according to external settings
		if (data->Scale != 1.0f)
		{
			BC.axis[0].Scale(data->Scale);
			BC.axis[1].Scale(data->Scale);
			BC.axis[2].Scale(data->Scale);
		}
		// compute transformation of world-space model vertices from reference
		// pose to desired pose
		BC.UnTransformCoords(data->RefCoordsInv, data->Transform);
#if 0
//!!
if (i == 32 || i == 34)
{
#define C BC
	DrawTextLeft("[%2d] : o=%8.3f %8.3f %8.3f", i, VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->Transform
	DrawTextLeft("TRN   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->RefCoordsInv
	DrawTextLeft("REF   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
}
#endif
	}
	unguard;
}


void CSkelMeshInstance::UpdateAnimation(float TimeDelta)
{
	const USkeletalMesh  *Mesh = GetMesh();
	const UMeshAnimation *Anim = Mesh->Animation;

	if (!Mesh->RefSkeleton.Num()) return;	// just in case

	// prepare bone-to-channel map
	//?? optimize: update when animation changed only
	for (int i = 0; i < Mesh->RefSkeleton.Num(); i++)
		BoneData[i].FirstChannel = 0;

	assert(MaxAnimChannel < MAX_SKELANIMCHANNELS);
	int Stage;
	CAnimChan *Chn;
	for (Stage = 0, Chn = Channels; Stage <= MaxAnimChannel; Stage++, Chn++)
	{
		if (Stage > 0 && Chn->AnimIndex1 == ANIM_UNASSIGNED)
			continue;
		// update tweening
		if (Chn->TweenTime)
		{
			Chn->TweenStep = TimeDelta / Chn->TweenTime;
			Chn->TweenTime -= TimeDelta;
			if (Chn->TweenTime < 0)
			{
				// stop tweening, start animation
				TimeDelta = -Chn->TweenTime;
				Chn->TweenTime = 0;
			}
			assert(Chn->Time == 0);
		}
		// note: TweenTime may be changed now, check again
		if (!Chn->TweenTime && Chn->AnimIndex1 >= 0)
		{
			// update animation time
			const FMeshAnimSeq *Seq1 = &Anim->AnimSeqs[Chn->AnimIndex1];
			const FMeshAnimSeq *Seq2 = (Chn->AnimIndex2 >= 0 && Chn->SecondaryBlend)
				? &Anim->AnimSeqs[Chn->AnimIndex2]
				: NULL;

			float Rate1 = Chn->Rate * Seq1->Rate;
			if (Seq2)
			{
				// if blending 2 channels, should adjust animation rate
				Rate1 = Lerp(Seq1->Rate / Seq1->NumFrames, Seq2->Rate / Seq2->NumFrames, Chn->SecondaryBlend)
					* Seq1->NumFrames;
			}
			Chn->Time += TimeDelta * Rate1;

			if (Chn->Looped)
			{
				if (Chn->Time >= Seq1->NumFrames)
				{
					// wrap time
					int numSkip = appFloor(Chn->Time / Seq1->NumFrames);
					Chn->Time -= numSkip * Seq1->NumFrames;
				}
			}
			else
			{
				if (Chn->Time >= Seq1->NumFrames-1)
				{
					// clamp time
					Chn->Time = Seq1->NumFrames-1;
					if (Chn->Time < 0)
						Chn->Time = 0;
				}
			}
		}
		// assign bones to channel
		if (Chn->BlendAlpha >= 1.0f && Stage > 0) // stage 0 already set
		{
			// whole subtree will be skipped in UpdateSkeleton(), so - mark root bone only
			BoneData[Chn->RootBone].FirstChannel = Stage;
		}
	}

	UpdateSkeleton();
}


/*-----------------------------------------------------------------------------
	Animation setup
-----------------------------------------------------------------------------*/

void CSkelMeshInstance::PlayAnimInternal(const char *AnimName, float Rate, float TweenTime, int Channel, bool Looped)
{
	guard(CSkelMeshInstance::PlayAnimInternal);

	CAnimChan &Chn = GetStage(Channel);
	if (Channel > MaxAnimChannel)
		MaxAnimChannel = Channel;

	int NewAnimIndex = FindAnim(AnimName);
	if (NewAnimIndex == INDEX_NONE)
	{
		// show default pose
		Chn.AnimIndex1     = INDEX_NONE;
		Chn.AnimIndex2     = INDEX_NONE;
		Chn.Time           = 0;
		Chn.Rate           = 0;
		Chn.Looped         = false;
		Chn.TweenTime      = TweenTime;
		Chn.SecondaryBlend = 0;
		return;
	}

	Chn.Rate   = Rate;
	Chn.Looped = Looped;

	if (NewAnimIndex == Chn.AnimIndex1 && Looped)
	{
		// animation not changed, just set some flags (above)
		return;
	}

	Chn.AnimIndex1     = NewAnimIndex;
	Chn.AnimIndex2     = INDEX_NONE;
	Chn.Time           = 0;
	Chn.SecondaryBlend = 0;
	Chn.TweenTime      = TweenTime;

	unguard;
}


void CSkelMeshInstance::SetBlendParams(int Channel, float BlendAlpha, const char *BoneName)
{
	guard(CSkelMeshInstance::SetBlendParams);
	CAnimChan &Chn = GetStage(Channel);
	Chn.BlendAlpha = BlendAlpha;
	if (Channel == 0)
		Chn.BlendAlpha = 1;		// force full animation for 1st stage
	Chn.RootBone = 0;
	if (BoneName)
	{
		Chn.RootBone = FindBone(BoneName);
		if (Chn.RootBone == INDEX_NONE)	// bone not found -- ignore animation
			Chn.BlendAlpha = 0;
	}
	unguard;
}


void CSkelMeshInstance::SetBlendAlpha(int Channel, float BlendAlpha)
{
	guard(CSkelMeshInstance::SetBlendAlpha);
	GetStage(Channel).BlendAlpha = BlendAlpha;
	unguard;
}


void CSkelMeshInstance::SetSecondaryAnim(int Channel, const char *AnimName)
{
	guard(CSkelMeshInstance::SetSecondaryAnim);
	CAnimChan &Chn = GetStage(Channel);
	Chn.AnimIndex2     = FindAnim(AnimName);
	Chn.SecondaryBlend = 0;
	unguard;
}


void CSkelMeshInstance::SetSecondaryBlend(int Channel, float BlendAlpha)
{
	guard(CSkelMeshInstance::SetSecondaryBlend);
	GetStage(Channel).SecondaryBlend = BlendAlpha;
	unguard;
}


void CSkelMeshInstance::AnimStopLooping(int Channel)
{
	guard(CSkelMeshInstance::AnimStopLooping);
	GetStage(Channel).Looped = false;
	unguard;
}


void CSkelMeshInstance::FreezeAnimAt(float Time, int Channel)
{
	guard(CSkelMeshInstance::FreezeAnimAt);
	CAnimChan &Chn = GetStage(Channel);
	Chn.Time = Time;
	Chn.Rate = 0;
	unguard;
}


void CSkelMeshInstance::GetAnimParams(int Channel, const char *&AnimName,
	float &Frame, float &NumFrames, float &Rate) const
{
	guard(CSkelMeshInstance::GetAnimParams);

	const USkeletalMesh  *Mesh = GetMesh();
	const UMeshAnimation *Anim = Mesh->Animation;
	const CAnimChan      &Chn  = GetStage(Channel);
	if (!Anim || Chn.AnimIndex1 < 0 || Channel > MaxAnimChannel)
	{
		AnimName  = "None";
		Frame     = 0;
		NumFrames = 0;
		Rate      = 0;
		return;
	}
	const FMeshAnimSeq &AnimSeq = Anim->AnimSeqs[Chn.AnimIndex1];
	AnimName  = AnimSeq.Name;
	Frame     = Chn.Time;
	NumFrames = AnimSeq.NumFrames;
	Rate      = AnimSeq.Rate * Chn.Rate;

	unguard;
}


/*-----------------------------------------------------------------------------
	Drawing
-----------------------------------------------------------------------------*/

static void GetBoneInfColor(int BoneIndex, CVec3 &Color)
{
	static float table[] = {0.1f, 0.4f, 0.7f, 1.0f};
	Color.Set(table[BoneIndex & 3], table[(BoneIndex >> 2) & 3], table[(BoneIndex >> 4) & 3]);
}


void CSkelMeshInstance::DrawSkeleton(bool ShowLabels)
{
	guard(CSkelMeshInstance::DrawSkeleton);

	const USkeletalMesh *Mesh = GetMesh();

	glDisable(GL_TEXTURE_2D);
	glLineWidth(3);
	glEnable(GL_LINE_SMOOTH);

	glBegin(GL_LINES);
	for (int i = 0; i < Mesh->RefSkeleton.Num(); i++)
	{
		const FMeshBone &B  = Mesh->RefSkeleton[i];
		const CCoords   &BC = BoneData[i].Coords;

		CVec3 v1;
		CVec3 Color;
		if (i > 0)
		{
//			Color.Set(1,1,0.3);
			//!! REMOVE LATER:
			int t = BoneUpdateCounts[i];
			Color.Set(t & 1, (t >> 1) & 1, (t >> 2) & 1);
			//!! ^^^^^^^^^^^^^
			v1 = BoneData[B.ParentIndex].Coords.origin;
		}
		else
		{
			Color.Set(1,0,1);
			v1.Zero();
		}
		if (ShowInfluences)
			GetBoneInfColor(i, Color);
		glColor3fv(Color.v);
		glVertex3fv(v1.v);
		glVertex3fv(BC.origin.v);

		if (ShowLabels)
		{
			// show bone label
			v1.Add(BC.origin);
			v1.Scale(0.5f);
			DrawText3D(v1, S_YELLOW"(%d)%s", i, *B.Name);
		}
	}
	glColor3f(1,1,1);
	glEnd();

	glLineWidth(1);
	glDisable(GL_LINE_SMOOTH);

	unguard;
}

void CSkelMeshInstance::DrawAttachments()
{
	guard(CSkelMeshInstance::DrawAttachments);

	const USkeletalMesh *Mesh = GetMesh();
	if (!Mesh->AttachAliases.Num()) return;

	glDisable(GL_TEXTURE_2D);

	glBegin(GL_LINES);
	for (int i = 0; i < Mesh->AttachAliases.Num(); i++)
	{
		const char *BoneName = Mesh->AttachBoneNames[i];
		int BoneIndex = FindBone(BoneName);
		if (BoneIndex == INDEX_NONE) continue;		// should not happen

		CCoords AC;
		BoneData[BoneIndex].Coords.UnTransformCoords((CCoords&)Mesh->AttachCoords[i], AC);

		for (int j = 0; j < 3; j++)
		{
			float color[3];
			CVec3 point0, point1;

			color[0] = color[1] = color[2] = 0.1f; color[j] = 1.0f;
			point0.Set(0, 0, 0); point0[j] = 10;
			AC.UnTransformPoint(point0, point1);

			glColor3fv(color);
			glVertex3fv(AC.origin.v);
			glVertex3fv(point1.v);
		}

		// show attachment label
		CVec3 labelOrigin;
		static const CVec3 origin0 = { 4, 4, 4 };
		AC.UnTransformPoint(origin0, labelOrigin);
		DrawText3D(labelOrigin, S_GREEN"%s\n(%s)", *Mesh->AttachAliases[i], BoneName);
	}
	glColor3f(1,1,1);
	glEnd();

	unguard;
}


void CSkelMeshInstance::TransformMesh(int NumInfs, const FVertInfluences *Infs, int NumVerts, const FVector *Verts, const CVec3 *Norms)
{
	guard(CSkelMeshInstance::TransformMesh);
	//?? try other tech: compute weighted sum of matrices, and then
	//?? transform vector (+ normal ?; note: normals requires normalized
	//?? transformations ?? (or normalization guaranteed by sum(weights)==1?))
	memset(MeshVerts,   0, sizeof(CVec3) * NumVerts);
	if (Norms)
		memset(MeshNormals, 0, sizeof(CVec3) * NumVerts);
#if CHECK_INFLUENCES
	float *infAccum = new float[NumVerts];
	memset(infAccum, 0, sizeof(float) * NumVerts);
#endif

#if 0
	//!!
	bool vertUsed[32768];
	memset(&vertUsed, 0, sizeof(vertUsed));
	int j;
	for (j = 0; j < NumInfs; j++)
	{
		int idx = Infs[j].PointIndex;
		if (Infs[j].BoneIndex == 34 && (Verts[idx].X < -27) && (Verts[idx].X > -28))
			vertUsed[idx] = true;
	}
#endif

	for (int i = 0; i < NumInfs; i++)
	{
		const FVertInfluences &Inf  = Infs[i];
		const CMeshBoneData   &data = BoneData[Inf.BoneIndex];
		int PointIndex = Inf.PointIndex;
		CVec3 tmp;
		// transform vertex
		data.Transform.UnTransformPoint((CVec3&)Verts[PointIndex], tmp);
		VectorMA(MeshVerts[PointIndex], Inf.Weight, tmp);
		// transform normal
		if (Norms)
		{
			data.Transform.axis.UnTransformVector(Norms[PointIndex], tmp);
			VectorMA(MeshNormals[PointIndex], Inf.Weight, tmp);
		}
#if CHECK_INFLUENCES
		if (Inf.Weight <= 0 || Inf.Weight > 1)
			DrawTextLeft("point[%d] have bad inf = %g", PointIndex, Inf.Weight);
		infAccum[PointIndex] += Inf.Weight;
#endif
#if 0
		//!!
		if (vertUsed[PointIndex])
			DrawTextLeft("%d: v[%g %g %g] b[%d] w[%g]", PointIndex, FVECTOR_ARG(Verts[PointIndex]), Inf.BoneIndex, Inf.Weight);
		else
			MeshVerts[PointIndex].Set(0, 0, 0);
#endif
	}
#if 0
	//!!
	DrawTextLeft("--------");
	for (j = 0; j < NumVerts; j++)
		if (vertUsed[j])
			DrawTextLeft("%5d: %7.2f %7.2f %7.2f", j, VECTOR_ARG(MeshVerts[j]));
#endif
#if CHECK_INFLUENCES
	for (int j = 0; j < NumVerts; j++)
		if (infAccum[j] < 0.9999f || infAccum[j] > 1.0001f)
			DrawTextLeft("point[%d] have bad inf sum = %g", j, infAccum[j]);
	delete infAccum;
#endif
	unguard;
}


void CSkelMeshInstance::DrawBaseSkeletalMesh(bool ShowNormals)
{
	guard(CSkelMeshInstance::DrawBaseSkeletalMesh);
	int i;

	const USkeletalMesh *Mesh = GetMesh();

#if 0
	glBegin(GL_POINTS);
	for (i = 0; i < Mesh->Points.Num(); i++)
		glVertex3fv(&Mesh->Points[i].X);
	glEnd();
	return;
#endif

	if (Mesh->VertInfluences.Num())		// condition is just in case
		TransformMesh(Mesh->VertInfluences.Num(), &Mesh->VertInfluences[0], Mesh->Points.Num(), &Mesh->Points[0], RefNormals);

	glEnable(GL_LIGHTING);
	int lastMatIndex = -1;	//!! implement same glBegin/glEnd/glBindTexture optimizations for VertMesh
	glBegin(GL_TRIANGLES);
	if (!ShowInfluences)
	{
		// draw normal mesh
		for (i = 0; i < Mesh->Triangles.Num(); i++)
		{
			const VTriangle &Face = Mesh->Triangles[i];
			if (Face.MatIndex != lastMatIndex)
			{
				// change active material
				glEnd();					// stop drawing sequence
				SetMaterial(Face.MatIndex);
				lastMatIndex = Face.MatIndex;
				glBegin(GL_TRIANGLES);		// restart drawing sequence
			}
			for (int j = 0; j < 3; j++)
			{
				const FMeshWedge &W = Mesh->Wedges[Face.WedgeIndex[j]];
				glTexCoord2f(W.TexUV.U, W.TexUV.V);
				glNormal3fv(MeshNormals[W.iVertex].v);
				glVertex3fv(MeshVerts[W.iVertex].v);
			}
		}
	}
	else
	{
		// draw influences
		if (!InfColors)
			BuildInfColors();
		assert(InfColors);

		glDisable(GL_TEXTURE_2D);
		for (i = 0; i < Mesh->Triangles.Num(); i++)
		{
			const VTriangle &Face = Mesh->Triangles[i];
			for (int j = 0; j < 3; j++)
			{
				const FMeshWedge &W = Mesh->Wedges[Face.WedgeIndex[j]];
				glNormal3fv(MeshNormals[W.iVertex].v);
				glColor3fv(InfColors[W.iVertex].v);
				glVertex3fv(MeshVerts[W.iVertex].v);
			}
		}
	}
	glEnd();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_LIGHTING);

#if SHOW_INFLUENCES
	glBegin(GL_LINES);
	for (i = 0; i < Mesh->VertInfluences.Num(); i++)
	{
		const FVertInfluences &Inf = Mesh->VertInfluences[i];
		int iBone = Inf.BoneIndex;
		const FMeshBone       &B   = Mesh->RefSkeleton[iBone];
		const CCoords         &BC  = BoneData[iBone].Coords;
		CVec3 Color;
		GetBoneInfColor(iBone, Color);
		glColor3fv(Color.v);
		glVertex3fv(MeshVerts[Inf.PointIndex].v);
		glVertex3fv(BC.origin.v);
	}
	glEnd();
#endif // SHOW_INFLUENCES

	// draw mesh normals
	if (ShowNormals)
	{
		glBegin(GL_LINES);
		glColor3f(0.5, 1, 0);
		for (i = 0; i < Mesh->Points.Num(); i++)
		{
			glVertex3fv(MeshVerts[i].v);
			CVec3 tmp;
			VectorMA(MeshVerts[i], 2, MeshNormals[i], tmp);
			glVertex3fv(tmp.v);
		}
		glEnd();
	}

	unguard;
}


void CSkelMeshInstance::DrawLodSkeletalMesh(const FStaticLODModel *lod)
{
	guard(CSkelMeshInstance::DrawLodSkeletalMesh);
	int i, sec;

	//!! Algorithm: similar to DrawBaseSkeletalMesh(): prepare MeshVerts array
	//!! using lod->Points and lod->VertInfluences; should build normals!
	//!! For Lineage should use FLineageWedge structure - it's enough; better -
	//!! - restore Points, Wedges and VertInfluences arrays using 'unk2'
	//!! (problem: should share verts between wedges); also restore mesh by lod#0

	TransformMesh(lod->VertInfluences.Num(), &lod->VertInfluences[0], lod->Points.Num(), &lod->Points[0], NULL);

/*	// skinning stream
	for (i = 0; i < lod->SkinningData.Num(); i++)
	{
		int n = lod->SkinningData[i];
		float &f = (float&)n;
		DrawTextRight("%4d : %08X : %10.3f", i, n, f);
	}
	// influences
	for (i = 0; i < lod->VertInfluences.Num(); i++)
	{
		const FVertInfluences &I = lod->VertInfluences[i];
		DrawTextLeft("b%4d : p%4d : %7.3f", I.BoneIndex, I.PointIndex, I.Weight);
	}
*/

	// smooth sections (influence count >= 2)
	for (sec = 0; sec < lod->SmoothSections.Num(); sec++)
	{
		const FSkelMeshSection &ms = lod->SmoothSections[sec];
		SetMaterial(ms.MaterialIndex);
		glBegin(GL_TRIANGLES);
#if 1
		for (i = 0; i < ms.NumFaces; i++)
		{
			const FMeshFace &F = lod->Faces[ms.FirstFace + i];
			//?? ignore F.MaterialIndex - may be any
//			assert(F.MaterialIndex == ms.MaterialIndex);
			for (int j = 0; j < 3; j++)
			{
				const FMeshWedge &W = lod->Wedges[F.iWedge[j]];
				glTexCoord2f(W.TexUV.U, W.TexUV.V);
//				glVertex3fv(&lod->SkinPoints[W.iVertex].Point.X);
				glVertex3fv(MeshVerts[W.iVertex].v);
			}
		}
#else
		for (i = 0; i < ms.NumFaces; i++)
		{
			for (int j = 0; j < 3; j++)
			{
				const FMeshWedge &W = lod->Wedges[lod->SmoothIndices.Indices[(ms.FirstFace + i) * 3 + j]];
				glTexCoord2f(W.TexUV.U, W.TexUV.V);
				glVertex3fv(&lod->SkinPoints[W.iVertex].Point.X);
			}
		}
#endif
		glEnd();
	}
	// rigid sections (influence count == 1)
	for (sec = 0; sec < lod->RigidSections.Num(); sec++)
	{
		const FSkelMeshSection &ms = lod->RigidSections[sec];
		SetMaterial(ms.MaterialIndex);
		glBegin(GL_TRIANGLES);
#if 1
		for (i = 0; i < ms.NumFaces; i++)
		{
//			const FMeshFace &F = lod->Faces[ms.FirstFace + i];
			for (int j = 0; j < 3; j++)
			{
//				const FMeshWedge &W = lod->Wedges[F.iWedge[j]];
				const FMeshWedge &W = lod->Wedges[lod->RigidIndices.Indices[(ms.FirstFace + i) * 3 + j]];
				glTexCoord2f(W.TexUV.U, W.TexUV.V);
//				glVertex3fv(&lod->Points[W.iVertex].X);
				glVertex3fv(MeshVerts[W.iVertex].v);
			}
		}
#else
		for (i = ms.MinStreamIndex; i < ms.MinStreamIndex + ms.NumStreamIndices; i++)
		{
			const FAnimMeshVertex &V = lod->VertexStream.Verts[lod->RigidIndices.Indices[i]];
			glTexCoord2f(V.Tex.U, V.Tex.V);
			glVertex3fv(&V.Pos.X);
		}
#endif
		glEnd();
	}
	unguard;
}


void CSkelMeshInstance::Draw()
{
	guard(CSkelMeshInstance::Draw);

	const USkeletalMesh *Mesh = GetMesh();

	// show skeleton
	if (ShowSkel)			//?? move this part to CSkelMeshViewer; call Inst->DrawSkeleton() etc
		DrawSkeleton(ShowLabels);
	// show mesh
	if (ShowSkel != 2)
	{
		if (LodNum < 0)
			DrawBaseSkeletalMesh(bShowNormals);
		else
			DrawLodSkeletalMesh(&Mesh->LODModels[LodNum]);
	}
	if (ShowAttach)
		DrawAttachments();

	unguard;
}


void CSkelMeshInstance::BuildInfColors()
{
	guard(CSkelMeshInstance::BuildInfColors);

	int i;

	const USkeletalMesh *Mesh = GetMesh();
	if (InfColors) delete InfColors;
	InfColors = new CVec3[Mesh->Points.Num()];

	// get colors for bones
	int NumBones = Mesh->RefSkeleton.Num();
	CVec3 BoneColors[MAX_MESHBONES];
	for (i = 0; i < NumBones; i++)
		GetBoneInfColor(i, BoneColors[i]);

	// process influences
	for (i = 0; i < Mesh->VertInfluences.Num(); i++)
	{
		const FVertInfluences &Inf = Mesh->VertInfluences[i];
		VectorMA(InfColors[Inf.PointIndex], Inf.Weight, BoneColors[Inf.BoneIndex]);
	}

	unguard;
}
