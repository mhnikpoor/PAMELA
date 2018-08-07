#include "Import/Eclipse_mesh.hpp"
#include <experimental/filesystem>
#include "Utils/StringUtils.hpp"
#include "Elements/ElementFactory.hpp"
#include "Elements/Element.hpp"
#include <map>
#include "Parallel/Communicator.hpp"
#include "Utils/File.hpp"
#include <iostream>

namespace PAMELA
{

	std::string Eclipse_mesh::m_label;
	int Eclipse_mesh::m_dimension = 0;
	int Eclipse_mesh::m_nvertices = 0;
	int Eclipse_mesh::m_nquadrilaterals = 0;
	int Eclipse_mesh::m_nhexahedra = 0;

	int Eclipse_mesh::m_nCOORD = 0;
	int Eclipse_mesh::m_nZCORN = 0;
	int Eclipse_mesh::m_nActiveCells = 0;
	int Eclipse_mesh::m_nTotalCells = 0;
	std::vector<int> Eclipse_mesh::m_SPECGRID = {0,0,0};
	std::vector<double>  Eclipse_mesh::m_COORD = {};
	std::vector<double>  Eclipse_mesh::m_ZCORN = {}; 
	std::vector<bool>  Eclipse_mesh::m_ACTNUM = {};
	std::vector<double> Eclipse_mesh::m_Duplicate_Element;

	std::unordered_map<ECLIPSE_MESH_TYPE, ELEMENTS::TYPE> Eclipse_mesh::m_TypeMap;
	std::unordered_map<std::string, std::vector<double>> Eclipse_mesh::m_Properties;

	void Eclipse_mesh::InitElementsMapping()
	{
		m_TypeMap[ECLIPSE_MESH_TYPE::EDGE] = ELEMENTS::TYPE::VTK_LINE;
		m_TypeMap[ECLIPSE_MESH_TYPE::HEXAHEDRON] = ELEMENTS::TYPE::VTK_HEXAHEDRON;
		m_TypeMap[ECLIPSE_MESH_TYPE::QUADRILATERAL] = ELEMENTS::TYPE::VTK_QUAD;
		m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX] = ELEMENTS::TYPE::VTK_VERTEX;
	};

	Mesh* Eclipse_mesh::CreateMesh(File file)
	{

		//Init map
		InitElementsMapping();

		//MPI
		auto irank = Communicator::worldRank();

		LOGINFO("*** Importing Eclipse mesh format file " + file.getNameWithoutExtension());

		std::ifstream mesh_file_;
		std::string file_content("A");
		int file_length = 0;

		std::vector<std::string> file_list;
		file_list.push_back(file.getFullName());
		int nfiles = 1;

		if (irank == 0)
		{
			//Open Main file
			mesh_file_.open(file.getFullName());
			ASSERT(mesh_file_.is_open(), file.getFullName() + " Could not be open");

			//Seek for include files
			std::istringstream mesh_file;
			file_content = { std::istreambuf_iterator<char>(mesh_file_), std::istreambuf_iterator<char>() };
			mesh_file_.close();
			mesh_file.str(file_content);
			std::string line,buffer;
			while (StringUtils::safeGetline(mesh_file, line))
			{
				StringUtils::RemoveStringAndFollowingContentFromLine("--", line);
				StringUtils::RemoveExtraSpaces(line);
				StringUtils::RemoveEndOfLine(line);
				StringUtils::RemoveTab(line);
				StringUtils::Trim(line);
				if (line == "INCLUDE")
				{
					mesh_file >> buffer;
					LOGINFO("---- Found Include file " + buffer + " found.");
					buffer = StringUtils::RemoveString("'", buffer);
					buffer = StringUtils::RemoveString("'", buffer);
					buffer = StringUtils::RemoveString("/", buffer);
					file_list.push_back(file.getDirectory() + "/"+buffer);
					nfiles++;
				}
			}
			mesh_file_.close();

		}

#ifdef WITH_MPI
		//Broadcast the mesh input (String)
		MPI_Bcast(&nfiles, 1, MPI_INT, 0, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
#endif

		for (auto ifile=0;ifile!=nfiles;++ifile)
		{

			std::string file_name;
			int file_length = 0;

			if (irank == 0)
			{
				file_name = file_list[ifile];
				file_length = static_cast<int>(file_name.size());
			}

#ifdef WITH_MPI
			//Broadcast the file name (String)
			MPI_Bcast(&file_length, 1, MPI_INT, 0, MPI_COMM_WORLD);
			file_name.resize(file_length);
			MPI_Bcast(&file_name[0], file_length, MPI_CHARACTER, 0, MPI_COMM_WORLD);
			MPI_Barrier(MPI_COMM_WORLD);
#endif

			if (irank == 0)
			{

				//Parse File content
				LOGINFO("---- Parsing " + file_name);
				file_content = StringUtils::FileToString(file_name);
				file_length = static_cast<int>(file_content.size());
			}

#ifdef WITH_MPI
			//Broadcast the file content (String)
			MPI_Bcast(&file_length, 1, MPI_INT, 0, MPI_COMM_WORLD);
			file_content.resize(file_length);
			MPI_Bcast(&file_content[0], file_length, MPI_CHARACTER, 0, MPI_COMM_WORLD);
			MPI_Barrier(MPI_COMM_WORLD);
#endif

			ParseString(file_content);

		}

		//Convert mesh into internal format
		LOGINFO("*** Converting into internal mesh format");
		auto mesh = ConvertMesh();

		//Fill mesh with imported properties
		LOGINFO("*** Filling mesh with imported properties");
		FillMeshWithProperties(mesh);

		return mesh;

	}


	Mesh* Eclipse_mesh::ConvertMesh()
	{
		Mesh* mesh = new UnstructuredMesh();
		ELEMENTS::TYPE elementType;
		std::vector<Point*> vertexTemp = { nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr };

		mesh->get_PolyhedronCollection()->addAndCreateGroup("POLYHEDRON_GROUP");

		int jb=0, kb=0, ib=0;
		int i0 = 0;
		int np;
		int nx= m_SPECGRID[0], ny = m_SPECGRID[1], nz = m_SPECGRID[2];
		double slope;

		int i_xm, i_xp;

		std::vector<double> z_pos(8, 0), y_pos(8, 0), x_pos(8, 0);

		std::vector<double> layer;
		std::vector<double> actnum;
		std::vector<double> duplicate_polyhedron;
		layer.reserve(nx*ny*nz);
		actnum.reserve(nx*ny*nz);
		m_Duplicate_Element.reserve(nx*ny*nz);

		int icellTotal = 0;
		int icell = 0;
		int cpt = 0;
		int ipoint = 0;
		for (auto k = 0; k != nz; ++k)
		{
			for (auto j = 0; j != ny; ++j)
			{
				for (auto i = 0; i != nx; ++i)
				{

						elementType = m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX];

						i_xm = 4 * nx*j + i * 2 + 8 * nx*ny*k;
						i_xp = 4 * nx*j + 2*nx + i * 2 + 8 * nx*ny*k;

						//z
						z_pos[0] = m_ZCORN[i_xm];
						z_pos[1] = m_ZCORN[i_xm + 1];
						z_pos[2] = m_ZCORN[i_xp];
						z_pos[3] = m_ZCORN[i_xp + 1];

						z_pos[4] = m_ZCORN[i_xm + 4 * nx*ny];
						z_pos[5] = m_ZCORN[i_xm + 1 + 4 * nx*ny];
						z_pos[6] = m_ZCORN[i_xp + 4 * nx*ny];
						z_pos[7] = m_ZCORN[i_xp + 1 + 4 * nx*ny];


						////Pillar 1
						np = i + (nx + 1)*j;

						//1
						i0 = np * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[0] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[0] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[0] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[0] = new Point(i, x_pos[0], y_pos[0], z_pos[0]);
					//vertexTemp[0] = mesh->addPoint(elementType, ipoint, "POINT_GROUP_0", x_pos[0], y_pos[0], z_pos[0]);

						//5
						i0 = np  * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[4] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[4] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[4] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[4] = new Point(i, x_pos[4], y_pos[4], z_pos[4]); 
					//vertexTemp[4] = mesh->addPoint(elementType, ipoint, "POINT_GROUP_0", x_pos[4], y_pos[4], z_pos[4]);

						////Pillar 2
						np = i + 1 + (nx + 1)*j;
						//2
						i0 = np  * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[1] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[1] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[1] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[1] = new Point(i, x_pos[1], y_pos[1], z_pos[1]); 
					//vertexTemp[1] = mesh->addPoint(elementType, ipoint, "POINT_GROUP_0", x_pos[1], y_pos[1], z_pos[1]);

						//6
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[5] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[5] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[5] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[5] = new Point(i, x_pos[5], y_pos[5], z_pos[5]); 
					//vertexTemp[5] = mesh->addPoint(elementType, ipoint, "POINT_GROUP_0", x_pos[5], y_pos[5], z_pos[5]);

						////Pillar 3
						np = i + (nx + 1)*(j+1);
						//3
						i0 = np  * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[2] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[2] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[2] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[3] = new Point(i, x_pos[2], y_pos[2], z_pos[2]); 
					//vertexTemp[3] = mesh->addPoint(elementType, i, "POINT_GROUP_0", x_pos[2], y_pos[2], z_pos[2]);

						//7
						i0 = np * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[6] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[6] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[6] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];
						
						//vertexTemp[7] = new Point(i, x_pos[6], y_pos[6], z_pos[6]); 
					//vertexTemp[7] = mesh->addPoint(elementType, i, "POINT_GROUP_0", x_pos[6], y_pos[6], z_pos[6]);

						////Pillar 4
						np = i + (nx + 1)*(j + 1)+1 ;
						//4
						i0 = np * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[3] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[3] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[3] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[2] = new Point(i, x_pos[3], y_pos[3], z_pos[3]); 
					//vertexTemp[2] = mesh->addPoint(elementType, i, "POINT_GROUP_0", x_pos[3], y_pos[3], z_pos[3]);

						//8
						i0 = np  * 6 - 1;
						if (m_COORD[i0 + 6] - m_COORD[i0 + 3] != 0)
						{
							slope = (z_pos[7] - m_COORD[i0 + 3]) / (m_COORD[i0 + 6] - m_COORD[i0 + 3]);
						}
						else
						{
							slope = 1;
						}
						x_pos[7] = slope*(m_COORD[i0 + 4] - m_COORD[i0 + 1]) + m_COORD[i0 + 1];
						y_pos[7] = slope*(m_COORD[i0 + 5] - m_COORD[i0 + 2]) + m_COORD[i0 + 2];

						//vertexTemp[6] = new Point(i, x_pos[7], y_pos[7], z_pos[7]); 
					//vertexTemp[6] = mesh->addPoint(elementType, i, "POINT_GROUP_0", x_pos[7], y_pos[7], z_pos[7]);
					

						if (m_ACTNUM[icellTotal])
						{
							icell++;

							//Points
							vertexTemp[0] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint-7, "POINT_GROUP_0", x_pos[0], y_pos[0], z_pos[0]);
							vertexTemp[4] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 6, "POINT_GROUP_0", x_pos[4], y_pos[4], z_pos[4]);
							vertexTemp[1] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 5, "POINT_GROUP_0", x_pos[1], y_pos[1], z_pos[1]);
							vertexTemp[5] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 4, "POINT_GROUP_0", x_pos[5], y_pos[5], z_pos[5]);
							vertexTemp[3] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 3, "POINT_GROUP_0", x_pos[2], y_pos[2], z_pos[2]);
							vertexTemp[7] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 2, "POINT_GROUP_0", x_pos[6], y_pos[6], z_pos[6]);
							vertexTemp[2] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 1, "POINT_GROUP_0", x_pos[3], y_pos[3], z_pos[3]);
							vertexTemp[6] = mesh->addPoint(m_TypeMap[ECLIPSE_MESH_TYPE::VERTEX], ipoint - 0, "POINT_GROUP_0", x_pos[7], y_pos[7], z_pos[7]);

							//Hexa
							mesh->get_PolyhedronCollection()->activeGroup("POLYHEDRON_GROUP");
							auto returned_element = mesh->addPolyhedron(m_TypeMap[ECLIPSE_MESH_TYPE::HEXAHEDRON], icell, "POLYHEDRON_GROUP", vertexTemp);

							if (returned_element != nullptr)
							{
								//m_Duplicate_Element[returned_element->get_globalIndex()] = 1;
								m_Duplicate_Element.push_back(2);
								cpt++;
							}
							else
							{
								m_Duplicate_Element.push_back(0);
							}

						}
				
						layer.push_back(k);
						actnum.push_back(m_ACTNUM[icellTotal]);
						icellTotal++;
					}
				}
			}

			layer.shrink_to_fit();
			actnum.shrink_to_fit();
			m_Duplicate_Element.shrink_to_fit();

			//Layers
			m_Properties["Layer"] = layer;
			m_Properties["ACTNUM"] = actnum;


			//Remove non-active properties
			auto iacthexas = icell;
			std::vector<double> temp; 
			for (auto it = m_Properties.begin(); it != m_Properties.end(); ++it)
			{
				if (it->first!="ACTNUM")
				{
					temp.clear();
					temp.reserve(iacthexas);
					auto& prop = it->second;
					for (size_t i = 0; i != prop.size(); ++i)
					{
						if ((actnum[i]) == 1)
						{
							temp.push_back(prop[i]);
						}
					}
					prop = temp;
				}
				
			}

			m_Properties.erase("ACTNUM");

			m_nActiveCells = iacthexas;
			m_nTotalCells = icellTotal;

			LOGINFO(std::to_string(icellTotal) + "  total hexas");
			LOGINFO(std::to_string(icell) + "  active hexas");
			LOGINFO(std::to_string(cpt) + "  duplicated hexas");

			return mesh;

	}

	void Eclipse_mesh::FillMeshWithProperties(Mesh* mesh)
	{
		//Temp var
		auto props = mesh->get_PolyhedronProperty();

		//Clean from duplicate elements
		//auto dim_g = m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2];
		auto cpt = 0;
			for (auto it = m_Properties.begin(); it != m_Properties.end(); ++it)
			{
				cpt = 0;
				for (auto i = 0; i != m_nActiveCells; ++i)
				{
					if (m_Duplicate_Element[i]==2)
					{
						(it->second)[i] = -987789;
					}
					else
					{
						cpt++;
					}
				}
				it->second.erase(std::remove(it->second.begin(), it->second.end(), -987789), it->second.end());
			}		

		//Transfer property to mesh object
		for (auto it = m_Properties.begin(); it != m_Properties.end(); ++it)
		{
			ASSERT(it->second.size() == props->get_Owner()->size_owned(), "Property set size is different from its owner");
			props->ReferenceProperty(it->first);
			props->SetProperty(it->first, it->second);
		}

		m_Properties["DUPLICATE"] = m_Duplicate_Element;
		m_Properties["DUPLICATE"].erase(std::remove(m_Properties["DUPLICATE"].begin(), m_Properties["DUPLICATE"].end(), 2), m_Properties["DUPLICATE"].end());
	}

	void Eclipse_mesh::ParseString(std::string& str)
	{
		std::istringstream mesh_file;
		mesh_file.str(str);
		std::string line,buffer;
		while (getline(mesh_file, line))
		{ 
			StringUtils::RemoveStringAndFollowingContentFromLine("--", line);
			StringUtils::RemoveExtraSpaces(line);
			StringUtils::RemoveEndOfLine(line);
			StringUtils::RemoveTab(line);
			StringUtils::Trim(line);
			if (line == "SPECGRID") 
			{
				LOGINFO("     o SPECGRID Found");
				std::vector<int> buf_int;
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::FromStringTo(buffer, buf_int);
				m_SPECGRID[0] = buf_int[0];
				m_SPECGRID[1] = buf_int[1];
				m_SPECGRID[2] = buf_int[2];
				m_nCOORD = 6 * (m_SPECGRID[1] + 1) * 6 * (m_SPECGRID[0] + 1);
				m_nZCORN = 8 * m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2];
				m_ZCORN.reserve(m_nZCORN);
				m_COORD.reserve(m_nCOORD);
				m_ACTNUM.reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
			}
			else if (line == "COORD")
			{
				LOGINFO("     o COORD Found");
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_COORD);
			}
			else if (line == "ZCORN")
			{
				LOGINFO("     o ZCORN Found");
				buffer = extractDataBelowKeyword(mesh_file);
				//StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_ZCORN);
			}
			else if (line == "ACTNUM")
			{
				LOGINFO("     o ACTNUM Found");
				m_Properties["ACTNUM"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_ACTNUM);
			}
			else if (line == "NNC")
			{
				LOGINFO("     o NNC Found");
				buffer = extractDataBelowKeyword(mesh_file);
			}
			else if (line == "PORO")
			{
				LOGINFO("     o PORO Found");
				m_Properties["PORO"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_Properties["PORO"]);
			}
			else if (line == "PERMX")
			{
				LOGINFO("     o PERMX Found");
				m_Properties["PERMX"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_Properties["PERMX"]);
			}
			else if (line == "PERMY")
			{
				LOGINFO("     o PERMY Found");
				m_Properties["PERMY"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_Properties["PERMY"]);
			}
			else if (line == "PERMZ")
			{
				LOGINFO("     o PERMZ Found");
				m_Properties["PERMZ"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_Properties["PERMZ"]);
			}
			else if (line == "NTG")
			{
				LOGINFO("     o NTG Found");
				m_Properties["NTG"].reserve(m_SPECGRID[0] * m_SPECGRID[1] * m_SPECGRID[2]);
				buffer = extractDataBelowKeyword(mesh_file);
				StringUtils::ExpandStarExpression(buffer);
				StringUtils::FromStringTo(buffer, m_Properties["NTG"]);
			}
		}

		ASSERT(m_SPECGRID[0]!= 0, "Mandatory SPECGRID keywords missing");

	}

	std::string Eclipse_mesh::extractDataBelowKeyword(std::istringstream& string_block)
	{
		char KeywordEnd = '/';
		std::string chunk;
		std::streampos pos;
		std::vector<std::string> res;
		getline(string_block, chunk, KeywordEnd);
		string_block.clear();
		StringUtils::RemoveTab(chunk);
		StringUtils::RemoveEndOfLine(chunk);
		StringUtils::Trim(chunk);
		res.push_back(chunk);
		string_block.ignore(10, '\n');
		getline(string_block, chunk);
		StringUtils::RemoveTab(chunk);
		StringUtils::RemoveEndOfLine(chunk);
		return res[0];
	}
}
