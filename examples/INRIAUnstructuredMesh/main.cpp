#include "PAMELA.hpp"
#include "Mesh/MeshFactory.hpp"
#include "Mesh/Mesh.hpp"
#include "Parallel/Communicator.hpp"
#include <thread>
#include "MeshDataWriters/MeshDataWriterFactory.hpp"
#ifdef WITH_VTK
#include <vtkMultiProcessController.h>
#include <vtkMPIController.h>
#endif

//#include "vld.h"

int main(int argc, char **argv) {

	using namespace  PAMELA;
        utils::pamela_unused(argc);
        utils::pamela_unused(argv);

	//std::this_thread::sleep_for(std::chrono::seconds(10));
	Communicator::initialize();
#ifdef WITH_VTK
	vtkSmartPointer<vtkMPIController> controler = vtkMPIController::New();
	controler->Initialize(&argc, &argv, true);
	vtkMultiProcessController::SetGlobalController(controler.Get());
#endif

  
	Mesh* MainMesh = MeshFactory::makeMesh(PAMELA_PATH"data/DataSets/Tiny/medit/tiny.mesh");

	MainMesh->CreateFacesFromCells();
	MainMesh->PerformPolyhedronPartitioning(ELEMENTS::FAMILY::POLYGON, ELEMENTS::FAMILY::POLYGON);
	MainMesh->CreateLineGroupWithAdjacency("TopologicalC2C", MainMesh->getAdjacencySet()->get_TopologicalAdjacency(ELEMENTS::FAMILY::POLYHEDRON, ELEMENTS::FAMILY::POLYHEDRON, ELEMENTS::FAMILY::POLYGON));
#ifdef WITH_VTK
	MeshDataWriter* OutputWriter = MeshDataWriterFactory::makeWriter(MainMesh, "UnstructuredGrid.vtm");
#else
	MeshDataWriter* OutputWriter = MeshDataWriterFactory::makeWriter(MainMesh, "UnstructuredGrid.case");
#endif
	OutputWriter->DeclareAndSetElementGlobalIndex();
	OutputWriter->DeclareAndSetPartitionNumber();

	OutputWriter->Init();

	//Dump variables
	OutputWriter->Dump();

	Communicator::finalize();

	return 0;
}
