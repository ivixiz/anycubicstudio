#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <IGESControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <ShapeFix_Shape.hxx>
#include <STEPControl_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void set_error(char *target, std::size_t capacity, const std::string &message) {
    if (target == nullptr || capacity == 0)
        return;
    const std::size_t count = std::min(capacity - 1, message.size());
    std::memcpy(target, message.data(), count);
    target[count] = '\0';
}

std::string lower_extension(const char *path) {
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

bool read_shape(const char *input, TopoDS_Shape &shape, std::string &error) {
    const std::string extension = lower_extension(input);
    if (extension == ".step" || extension == ".stp") {
        STEPControl_Reader reader;
        if (reader.ReadFile(input) != IFSelect_RetDone) {
            error = "OpenCascade could not read the STEP file";
            return false;
        }
        if (reader.TransferRoots() == 0) {
            error = "OpenCascade STEP file contains no transferable roots";
            return false;
        }
        shape = reader.OneShape();
    } else if (extension == ".iges" || extension == ".igs") {
        IGESControl_Reader reader;
        if (reader.ReadFile(input) != IFSelect_RetDone) {
            error = "OpenCascade could not read the IGES file";
            return false;
        }
        if (reader.TransferRoots() == 0) {
            error = "OpenCascade IGES file contains no transferable roots";
            return false;
        }
        shape = reader.OneShape();
    } else {
        error = "OpenCascade importer received an unsupported CAD extension";
        return false;
    }
    if (shape.IsNull()) {
        error = "OpenCascade imported a null CAD shape";
        return false;
    }
    return true;
}

void print_topology(const char *stage, const TopoDS_Shape &shape) {
    if (std::getenv("ANYCUBIC_OCCT_DIAGNOSTICS") == nullptr)
        return;
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    int free_edges = 0;
    int non_manifold_edges = 0;
    for (int index = 1; index <= edge_faces.Extent(); ++index) {
        const int faces = edge_faces.FindFromIndex(index).Extent();
        free_edges += faces == 1;
        non_manifold_edges += faces > 2;
        if (faces == 1) {
            BRepAdaptor_Curve curve(TopoDS::Edge(edge_faces.FindKey(index)));
            const gp_Pnt first = curve.Value(curve.FirstParameter());
            const gp_Pnt last = curve.Value(curve.LastParameter());
            const double length = GCPnts_AbscissaPoint::Length(curve);
            std::cerr << "OCCT " << stage << " free edge " << index
                      << ": length=" << length << " from=(" << first.X()
                      << ',' << first.Y() << ',' << first.Z() << ") to=("
                      << last.X() << ',' << last.Y() << ',' << last.Z()
                      << ")\n";
        }
    }
    auto count = [&shape](TopAbs_ShapeEnum type) {
        int result = 0;
        for (TopExp_Explorer iterator(shape, type); iterator.More(); iterator.Next())
            ++result;
        return result;
    };
    std::cerr << "OCCT " << stage << ": solids=" << count(TopAbs_SOLID)
              << " shells=" << count(TopAbs_SHELL)
              << " faces=" << count(TopAbs_FACE)
              << " edges=" << edge_faces.Extent()
              << " free_edges=" << free_edges
              << " non_manifold_edges=" << non_manifold_edges << '\n';
}

bool validate_closed_shape(const TopoDS_Shape &shape, double tolerance,
                           std::string &error) {
    TopTools_IndexedDataMapOfShapeListOfShape edge_faces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edge_faces);
    int open_edges = 0;
    int non_manifold_edges = 0;
    double longest_open_edge = 0.0;
    for (int index = 1; index <= edge_faces.Extent(); ++index) {
        const int faces = edge_faces.FindFromIndex(index).Extent();
        if (faces > 2) {
            ++non_manifold_edges;
        } else if (faces == 1) {
            BRepAdaptor_Curve curve(TopoDS::Edge(edge_faces.FindKey(index)));
            const double length = GCPnts_AbscissaPoint::Length(curve);
            if (length > tolerance) {
                ++open_edges;
                longest_open_edge = std::max(longest_open_edge, length);
            }
        }
    }
    if (open_edges == 0 && non_manifold_edges == 0)
        return true;
    error = "CAD B-Rep remains open/non-manifold after healing: " +
            std::to_string(open_edges) + " open edge(s), " +
            std::to_string(non_manifold_edges) +
            " non-manifold edge(s), longest open edge " +
            std::to_string(longest_open_edge) + " mm";
    return false;
}

} // namespace

extern "C" int anycubic_occt_tessellate(const char *input, const char *output,
                                         double linear_deflection,
                                         double angular_deflection, char *error,
                                         std::size_t error_capacity) noexcept {
    try {
        if (input == nullptr || output == nullptr || linear_deflection <= 0.0 ||
            angular_deflection <= 0.0) {
            set_error(error, error_capacity, "invalid OpenCascade tessellation arguments");
            return 1;
        }
        TopoDS_Shape shape;
        std::string read_error;
        if (!read_shape(input, shape, read_error)) {
            set_error(error, error_capacity, read_error);
            return 2;
        }
        print_topology("imported", shape);

        // STEP/IGES exporters sometimes store touching faces as independent
        // shells. Sew them before triangulation so a shared geometric edge is
        // represented by a shared topological edge. The tolerance is bounded
        // to a few output pixels: larger defects remain explicit errors in the
        // contour builder rather than being silently filled.
        const double healing_tolerance =
            std::clamp(linear_deflection * 8.0, 1.0e-5, 0.075);
        BRepBuilderAPI_Sewing sewing(healing_tolerance, Standard_True,
                                     Standard_True, Standard_True,
                                     Standard_False);
        sewing.Add(shape);
        sewing.Perform();
        if (!sewing.SewedShape().IsNull())
            shape = sewing.SewedShape();
        print_topology("sewn", shape);

        // Heal the sewn B-Rep before triangulation. This is deliberately done
        // here instead of hiding arbitrary cracks in 2D sliced contours.
        Handle(ShapeFix_Shape) fixer = new ShapeFix_Shape(shape);
        fixer->SetPrecision(std::max(1.0e-7, linear_deflection * 0.02));
        fixer->SetMaxTolerance(healing_tolerance);
        fixer->Perform();
        shape = fixer->Shape();
        print_topology("fixed", shape);
        BRepTools::Clean(shape);
        std::string topology_error;
        if (!validate_closed_shape(shape, healing_tolerance, topology_error)) {
            set_error(error, error_capacity, topology_error);
            return 3;
        }

        BRepMesh_IncrementalMesh mesh(shape, linear_deflection, Standard_False,
                                      angular_deflection, Standard_True);
        mesh.Perform();
        if (!mesh.IsDone()) {
            set_error(error, error_capacity, "OpenCascade B-Rep meshing did not complete");
            return 4;
        }

        StlAPI_Writer writer;
        writer.ASCIIMode() = Standard_False;
        if (!writer.Write(shape, output)) {
            set_error(error, error_capacity, "OpenCascade could not write tessellated STL");
            return 5;
        }
        set_error(error, error_capacity, "");
        return 0;
    } catch (const Standard_Failure &failure) {
        set_error(error, error_capacity,
                  std::string("OpenCascade failure: ") + failure.GetMessageString());
        return 6;
    } catch (const std::exception &failure) {
        set_error(error, error_capacity,
                  std::string("CAD tessellation failure: ") + failure.what());
        return 7;
    } catch (...) {
        set_error(error, error_capacity, "unknown CAD tessellation failure");
        return 8;
    }
}
