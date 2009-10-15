#include "Core.h"
#include "UnrealClasses.h"

#include "ObjectViewer.h"
#include "../MeshInstance/MeshInstance.h"


CMeshViewer::~CMeshViewer()
{
	delete Inst;
}


void CMeshViewer::Draw3D()
{
	guard(CMeshViewer::Draw3D);
	assert(Inst);

	// draw axis
	BindDefaultMaterial(true);
	glBegin(GL_LINES);
	for (int i = 0; i < 3; i++)
	{
		CVec3 tmp = nullVec3;
		tmp[i] = 1;
		glColor3fv(tmp.v);
		tmp[i] = 70;
		glVertex3fv(tmp.v);
		glVertex3fv(nullVec3.v);
	}
	glEnd();
	glColor3f(1, 1, 1);

	// draw mesh
	glPolygonMode(GL_FRONT_AND_BACK, Inst->bWireframe ? GL_LINE : GL_FILL);	//?? bWireframe is inside Inst, but used here only ?
	Inst->Draw();

	// restore draw state
	glColor3f(1, 1, 1);
	BindDefaultMaterial(true);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	unguard;
}


void CMeshViewer::ShowHelp()
{
	CObjectViewer::ShowHelp();
	DrawTextLeft("N           show normals\n"
				 "W           toggle wireframe\n"
				 "M           colorize materials\n");
}


void CMeshViewer::ProcessKey(int key)
{
	switch (key)
	{
	case 'n':
		Inst->bShowNormals = !Inst->bShowNormals;
		break;

	case 'm':
		Inst->bColorMaterials = !Inst->bColorMaterials;
		break;

	case 'w':
		Inst->bWireframe = !Inst->bWireframe;
		break;

	default:
		CObjectViewer::ProcessKey(key);
	}
}
