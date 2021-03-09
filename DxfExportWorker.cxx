
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING 1

#include "DxfExportWorker.hxx"

#include <experimental/filesystem>

#include <uf.h>
#include <uf_defs.h>
#include <NXOpen/Session.hxx>
#include <NXOpen/BasePart.hxx>
#include <NXOpen/Part.hxx>
#include <NXOpen/PartCloseResponses.hxx>
#include <NXOpen/PartCollection.hxx>
#include <NXOpen/PartLoadStatus.hxx>
#include <NXOpen/DexBuilder.hxx>
#include <NXOpen/DexManager.hxx>
#include <NXOpen/DxfdwgCreator.hxx>

#include <NXOpen/Body.hxx>
#include <NXOpen/BodyCollection.hxx>
#include <NXOpen/CartesianCoordinateSystem.hxx>
#include <NXOpen/CoordinateSystemCollection.hxx>
#include <NXOpen/Edge.hxx>
#include <NXOpen/Face.hxx>
#include <NXOpen/Features_DatumCsys.hxx>
#include <NXOpen/Features_DatumCsysBuilder.hxx>
#include <NXOpen/Features_FeatureCollection.hxx>
#include <NXOpen/Point.hxx>
#include <NXOpen/PointCollection.hxx>
#include <NXOpen/Sketch.hxx>
#include <NXOpen/SketchCollection.hxx>
#include <NXOpen/View.hxx>
#include <NXOpen/ViewCollection.hxx>
#include <NXOpen/Xform.hxx>
#include <NXOpen/XformCollection.hxx>

#include <NXOpen/DraftingManager.hxx>
#include <NXOpen/SelectDisplayableObject.hxx>
#include <NXOpen/Annotations_Annotation.hxx>
#include <NXOpen/Annotations_AnnotationManager.hxx>
#include <NXOpen/Annotations_LeaderBuilder.hxx>
#include <NXOpen/Annotations_LeaderData.hxx>
#include <NXOpen/Annotations_LeaderDataList.hxx>
#include <NXOpen/Annotations_OriginBuilder.hxx>

#include <NXOpen/LogFile.hxx>
#include <NXOpen/LicenseManager.hxx>
#include <NXOpen/NXObject.hxx>
#include <NXOpen/NXException.hxx>
#include <NXOpen/SelectNXObjectList.hxx>

using namespace NXOpen;
using namespace std;

namespace fs = experimental::filesystem;

/* Initialize static variables */
Session *(DxfExportWorker::nx_session) = NULL;
LogFile *(DxfExportWorker::nx_system_log) = NULL;

DxfExportWorker::DxfExportWorker()
{
    DxfExportWorker::nx_system_log->WriteLine("\n\t\t\t*********************************");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*    NXOpen Dxf Export Begin    *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*********************************\n");

    /* init class members */
    nx_session = Session::GetSession();
    nx_system_log = nx_session->LogFile();

    // get solid_modeling license
    DxfExportWorker::nx_session->LicenseManager()->Reserve("solid_modeling", nullptr);
    
    part = nullptr;
    dxf_factory = nullptr;
    selected_objects = nullptr;
}

DxfExportWorker::~DxfExportWorker()
{
    // release solid_modeling license
    DxfExportWorker::nx_session->LicenseManager()->Release("solid_modeling", nullptr);
    
    /* close factory, if not null */
    if (dxf_factory)
        dxf_factory->Destroy();
        delete selected_objects;
        delete dxf_factory;

    DxfExportWorker::nx_system_log->WriteLine("\n\t\t\t*********************************");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*     NXOpen Dxf Export End     *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*********************************\n");
}

void DxfExportWorker::init_factory() {
    /* init dxf/dwg exporter */
    dxf_factory = nx_session->DexManager()->CreateDxfdwgCreator();

    dxf_factory->SetSettingsFile(DXF_EXPORT_CONFIG);
    dxf_factory->SetExportData(DxfdwgCreator::ExportDataOptionDrawing);
    dxf_factory->SetViewEditMode(true);
    dxf_factory->SetFlattenAssembly(true);
    dxf_factory->SetExportAs(DxfdwgCreator::ExportAsOptionThreeD);
    dxf_factory->ExportSelectionBlock()->SetSelectionScope(ObjectSelector::ScopeSelectedObjects);
    dxf_factory->SetExportFacesAs(DxfdwgCreator::ExportFacesAsOptionsPolylineMesh);
    dxf_factory->SetProcessHoldFlag(true);

    /* set up dxf/dwg export for file */
    dxf_factory->SetInputFile(part->FullPath().GetText());
    
    selected_objects = dxf_factory->ExportSelectionBlock()->SelectionComp();
}

void DxfExportWorker::process_part(const char *part_file_name)
{
    /* Open part */
    PartLoadStatus *part_load_status;
    part = dynamic_cast<Part *>(DxfExportWorker::nx_session->Parts()->OpenActiveDisplay(part_file_name, DisplayPartOptionReplaceExisting, &part_load_status));
    delete part_load_status;

    // enter modeling
    nx_session->ApplicationSwitchImmediate("UG_APP_MODELING");

    process_part();

    /* close part */
    part->Close(BasePart::CloseWholeTreeTrue, BasePart::CloseModifiedCloseModified, nullptr);
    nx_session->ApplicationSwitchImmediate("UG_APP_NOPART");
    
    /* clean up part objects */
    delete part;
}

void DxfExportWorker::process_part()
{
    // create dxf exporter
    //  this is per part, not per session
    init_factory();

    // ********************
    // *     DO STUFF     *
    // ********************
    add_sketches();
    export_bodies(); 

    /* reset dxf/dwg exporter object selection */
    dxf_factory->Destroy();
    selected_objects = nullptr;
    dxf_factory = nullptr;
}

void DxfExportWorker::add_sketches()
{
    nx_system_log->WriteLine("\n\t***********************");
    nx_system_log->WriteLine(  "\t*   Adding Sketches   *");
    nx_system_log->WriteLine(  "\t***********************\n");

    /* Add ZINC sketches */
    for (Sketch *sketch: *(part->Sketches()))
    {
        /* add ZINC sketches */
        if (strstr(sketch->Name().GetText(), "ZINC"))
        {
            /* sketch is hidden: skip */
            if (sketch->IsBlanked())
            {
                nx_system_log->Write(" - Skipping blanked sketch: ");
                nx_system_log->Write(sketch->Name().GetText());
            }

            /* sketch is visible: add to file */
            else
            {
                nx_system_log->Write(" + Adding sketch: ");
                nx_system_log->Write(sketch->Name().GetText());
                nx_system_log->Write(" -> ");

                /* add sketch lines to sketches */
                if (selected_objects->Add(sketch->GetAllGeometry()))
                    nx_system_log->Write("OK");
                else
                    nx_system_log->Write("FAILED");
            }

            nx_system_log->WriteLine("");
        }
    }

}

void DxfExportWorker::export_bodies()
{
    nx_system_log->WriteLine("\n\t***********************");
    nx_system_log->WriteLine(  "\t*    Adding Bodies    *");
    nx_system_log->WriteLine(  "\t***********************\n");

    /* build base part file name */
    string base_save_file_name(DXF_OUTPUT_DIR);
    base_save_file_name.append(part->GetStringAttribute("JobNo").GetText());
    base_save_file_name.append("_");
    base_save_file_name.append(part->GetStringAttribute("Mark").GetText());
    base_save_file_name.append("-");

    string body_name;

    /* Add body to dxf export */
    for ( Body *body: *( part->Bodies() ) )
    {
        // TODO: Body name inference if empty
        /* build file name */
        body_name.assign(body->Name().GetText());
        
        /* Set output file name */
        /* {DXF_OUTPUT_DIR}\{Job}_{Girder}-{BodyName}.dxf */
        dxf_factory->SetOutputFile(base_save_file_name + body_name + ".dxf");

        /* export body */
        nx_system_log->Write("\nExporting body: ");
        nx_system_log->WriteLine(body_name);

        /* add body to export */
        bool added = selected_objects->Add(body);

        handle_thickness(body);

        /* 
            delete part file if it exists
            this seems to speed up export compared to overwriting files
            also, it keeps from accumulating *_bk.dxf files
        */
        if (fs::exists(fs::path(dxf_factory->OutputFile().GetText())))
            remove(dxf_factory->OutputFile().GetText());
            
        /* generate DXF file */
        NXObject *generate_result = dxf_factory->Commit();
        
        /* delete added body (so that it does not export next time) */
        selected_objects->Remove(body);
    }
}

NXObject *DxfExportWorker::add_annotation(double thickness)
{
    // swithc to drafting
    nx_session->ApplicationSwitchImmediate("UG_APP_DRAFTING");
    
    part->Drafting()->EnterDraftingApplication();
    part->Views()->WorkView()->UpdateCustomSymbols();
    part->Drafting()->SetTemplateInstantiationIsComplete(true);
    
    // turn off drawing layout (allows drafting tools in modeling)
    part->Drafting()->SetDrawingLayout(false);
    
    Annotations::DraftingNoteBuilder *note_factory;
    Annotations::SimpleDraftingAid *drafting_aid(NULL);
    note_factory = part->Annotations()->CreateDraftingNoteBuilder(drafting_aid);
    
    note_factory->Origin()->SetAnchor(Annotations::OriginBuilder::AlignmentPositionTopLeft);
    
    vector<NXString> annotations(1);
    annotations[0] = "THICKNESS: " + to_string(thickness);

    note_factory->Text()->TextBlock()->SetText(annotations);
    note_factory->Origin()->Plane()->SetPlaneMethod(Annotations::PlaneBuilder::PlaneMethodTypeXyPlane);
    
    // set leader settings
    Annotations::LeaderData *leader_data = part->Annotations()->CreateLeaderData();
    leader_data->SetArrowhead(Annotations::LeaderData::ArrowheadTypeFilledArrow);
    leader_data->SetVerticalAttachment(Annotations::LeaderVerticalAttachmentCenter);
    leader_data->SetStubSide(Annotations::LeaderSideInferred);
    note_factory->Leader()->Leaders()->Append(leader_data);

    // text size
    note_factory->Style()->LetteringStyle()->SetGeneralTextSize(5.0);
    
    Annotations::Annotation::AssociativeOriginData note_origin;
    Annotations::Annotation *note_annotation(NULL);
    View *note_view(NULL);
    Point *note_point(NULL);

    // set origin settings
    note_origin.OriginType = Annotations::AssociativeOriginTypeDrag;
    note_origin.View = note_view;
    note_origin.ViewOfGeometry = note_view;
    note_origin.PointOnGeometry = note_point;
    note_origin.VertAnnotation = note_annotation;
    note_origin.VertAlignmentPosition = Annotations::AlignmentPositionTopLeft;
    note_origin.HorizAnnotation = note_annotation;
    note_origin.HorizAlignmentPosition = Annotations::AlignmentPositionTopLeft;
    note_origin.AlignedAnnotation = note_annotation;
    note_origin.DimensionLine = 0;
    note_origin.AssociatedView = note_view;
    note_origin.AssociatedPoint = note_point;
    note_origin.OffsetAnnotation = note_annotation;
    note_origin.OffsetAlignmentPosition = Annotations::AlignmentPositionTopLeft;
    note_origin.XOffsetFactor = 0.0;
    note_origin.YOffsetFactor = 0.0;
    note_origin.StackAlignmentPosition = Annotations::StackAlignmentPositionAbove;

    note_factory->Origin()->SetAssociativeOrigin(note_origin);
    
    // set note location
    Point3d note_location(0.0, -20.0, 0.0);
    note_factory->Origin()->Origin()->SetValue(NULL, note_view, note_location);

    // create note
    NXObject *commit_result = note_factory->Commit();
    note_factory->Destroy();
    
    // switch back to modeling
    nx_session->ApplicationSwitchImmediate("UG_APP_MODELING");

    return commit_result;
}

void DxfExportWorker::handle_thickness(Body *body)
{
    Point *p = nullptr;
    double min_z = 0.0;
    double max_z = 0.0;
    double thk;
    NXObject *note;

    for (Edge *e: body->GetEdges())
    {
        try
        {
            // create center point on edge
            p = part->Points()->CreatePoint(e, SmartObject::UpdateOptionWithinModeling);

            min_z = min(p->Coordinates().Z, min_z);
            max_z = max(p->Coordinates().Z, max_z);

            // part->Points()->DeletePoint(p);
        }
        // some points will error out, don't care
        catch (const exception &ex){}
    }

    // change WCS if origin Z is below 0
    if (min_z != 0.0) {
        thk = abs(max_z - min_z);
        note = add_annotation(thk);

        bool added = selected_objects->Add(note);
    }

}

void DxfExportWorker::set_wcs_to_face(double new_z)
{
    Features::Feature *nullNXOpen_Features_Feature(NULL);
    Features::DatumCsysBuilder *datum_csys_builder = part->Features()->CreateDatumCsysBuilder(nullNXOpen_Features_Feature);
    
    Point3d origin(0.0, 0.0, new_z);
    Vector3d x_dir(1.0, 0.0, 0.0);
    Vector3d y_dir(0.0, 1.0, 0.0);
    Xform *xform = part->Xforms()->CreateXform(origin, x_dir, y_dir, SmartObject::UpdateOptionWithinModeling, 1.0);

    CartesianCoordinateSystem *ccord_sys = part->CoordinateSystems()->CreateCoordinateSystem(xform, SmartObject::UpdateOptionWithinModeling);

    /* create datum CSYS */
    nx_system_log->Write("Creating new datum csys at z-level: ");
    nx_system_log->WriteLine(to_string(new_z));

    datum_csys_builder->SetCsys(ccord_sys);
    NXObject *csys_commit = datum_csys_builder->Commit();

    Features::DatumCsys *datum_csys(dynamic_cast<Features::DatumCsys *>(csys_commit));
    datum_csys->SetWcsAtCsys();

    /* clean up pointers and builder */
    delete xform;
    delete ccord_sys;
    delete csys_commit;
    datum_csys_builder->Destroy();
}
