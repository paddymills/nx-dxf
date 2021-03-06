
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING 1

#include "DxfExportWorker.hxx"
#include "BodyBoundary.hxx"

#include <experimental/filesystem>
#include <map>

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
#include <NXOpen/Edge.hxx>
#include <NXOpen/Face.hxx>
#include <NXOpen/Point.hxx>
#include <NXOpen/PointCollection.hxx>
#include <NXOpen/Sketch.hxx>
#include <NXOpen/SketchCollection.hxx>
#include <NXOpen/View.hxx>
#include <NXOpen/ViewCollection.hxx>

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

// Initialize static variables
Session *(DxfExportWorker::nx_session) = NULL;
LogFile *(DxfExportWorker::nx_system_log) = NULL;

DxfExportWorker::DxfExportWorker()
{
    DxfExportWorker::nx_system_log->WriteLine("\n\t\t\t*********************************");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*    NXOpen Dxf Export Begin    *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*********************************\n");

    // init class members
    nx_session = Session::GetSession();
    nx_system_log = nx_session->LogFile();

    // get solid_modeling license
    DxfExportWorker::nx_session->LicenseManager()->Reserve("solid_modeling", nullptr);
    
    part = nullptr;
    dxf_factory = nullptr;
}

DxfExportWorker::~DxfExportWorker()
{
    // release solid_modeling license
    DxfExportWorker::nx_session->LicenseManager()->Release("solid_modeling", nullptr);
    
    // close factory, if not null
    if (dxf_factory)
        dxf_factory->Destroy();
        delete dxf_factory;

    DxfExportWorker::nx_system_log->WriteLine("\n\t\t\t*********************************");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*     NXOpen Dxf Export End     *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*                               *");
    DxfExportWorker::nx_system_log->WriteLine(  "\t\t\t*********************************\n");
}

void DxfExportWorker::init_factory() {
    // init dxf/dwg exporter
    dxf_factory = nx_session->DexManager()->CreateDxfdwgCreator();

    dxf_factory->SetSettingsFile(DXF_EXPORT_CONFIG);
    dxf_factory->SetExportData(DxfdwgCreator::ExportDataOptionDrawing);
    dxf_factory->SetViewEditMode(true);
    dxf_factory->SetFlattenAssembly(true);
    dxf_factory->SetExportAs(DxfdwgCreator::ExportAsOptionThreeD);
    dxf_factory->ExportSelectionBlock()->SetSelectionScope(ObjectSelector::ScopeSelectedObjects);
    dxf_factory->SetExportFacesAs(DxfdwgCreator::ExportFacesAsOptionsPolylineMesh);
    dxf_factory->SetProcessHoldFlag(true);

    // set up dxf/dwg export for file
    dxf_factory->SetInputFile(part->FullPath().GetText());
}

void DxfExportWorker::process_part(const char *part_file_name)
{
    // Open part
    PartLoadStatus *part_load_status;
    part = dynamic_cast<Part *>(DxfExportWorker::nx_session->Parts()->OpenActiveDisplay(part_file_name, DisplayPartOptionReplaceExisting, &part_load_status));
    delete part_load_status;

    // enter modeling
    nx_session->ApplicationSwitchImmediate("UG_APP_MODELING");

    process_part();

    // close part
    part->Close(BasePart::CloseWholeTreeTrue, BasePart::CloseModifiedCloseModified, nullptr);
    nx_session->ApplicationSwitchImmediate("UG_APP_NOPART");
    
    // clean up part objects
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
    handle_part_properties();
    add_sketches();
    export_bodies(); 

    // reset dxf/dwg exporter object selection
    dxf_factory->Destroy();
    dxf_factory = nullptr;
}

void DxfExportWorker::handle_part_properties()
{
    string text;

    // drawing
    text = part->GetStringAttribute("DWG_NUMBER").GetText();
    if (!is_empty_property(text))
        annotations["DRAWING"] = text;

}

void DxfExportWorker::add_sketches()
{
    nx_system_log->WriteLine("\n\t***********************");
    nx_system_log->WriteLine(  "\t*   Adding Sketches   *");
    nx_system_log->WriteLine(  "\t***********************\n");

    // Add ZINC sketches
    for (Sketch *sketch: *(part->Sketches()))
    {
        // add ZINC sketches
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

                // add sketch lines to sketches
                if (add_object_to_export(sketch->GetAllGeometry()))
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

    // get filename (no directories or extensions)
    fs::path *part_path = new fs::path(part->FullPath().GetText());
    string part_filename(part_path->filename().stem().string());
    nx_system_log->Write("!! FILE NAME !!  ");
    nx_system_log->WriteLine(part_filename);

    // build base part file name
    string part_name;
    part_name.append(part->GetStringAttribute("JobNo").GetText());
    part_name.append("_");
    part_name.append(part->GetStringAttribute("Mark").GetText());

    // make sure part filename starts with build part_name
    // could be stale data
    if (part_filename.rfind(part_name, 0) != 0)
        part_name = part_filename;

    string body_name;
    int body_name_counter = 1;

    // Add body to dxf export 
    bool has_multiple_bodies = (part->Bodies()->begin() == part->Bodies()->end());
    for ( Body *body: *( part->Bodies() ) )
    {
        // get name of body
        body_name.assign(body->Name().GetText());
        
        // Set output file name
        // single body part
        if (has_multiple_bodies)
            dxf_factory->SetOutputFile(DXF_OUTPUT_DIR + part_name + ".dxf");
        
        // multiple body part, but body is not named
        // {DXF_OUTPUT_DIR}\{Job}_{Girder}-web_1.dxf
        else if (body_name.empty())
            dxf_factory->SetOutputFile(DXF_OUTPUT_DIR + part_filename + "_" + to_string(body_name_counter++) + ".dxf");

        // multiple body part, named body
        // {DXF_OUTPUT_DIR}\{Job}_{Girder}-{BodyName}.dxf
        else
            dxf_factory->SetOutputFile(DXF_OUTPUT_DIR + part_name + "-" + body_name + ".dxf");
        

        // export body
        nx_system_log->Write("\nExporting body: ");
        nx_system_log->WriteLine(body_name);

        // add body to export
        add_purgeable_object_to_export(body);

        handle_body(body);

        /* 
            delete part file if it exists
            this seems to speed up export compared to overwriting files
            also, it keeps from accumulating *_bk.dxf files
        */
        if (fs::exists(fs::path(dxf_factory->OutputFile().GetText())))
            remove(dxf_factory->OutputFile().GetText());
            
        // generate DXF file
        NXObject *generate_result = dxf_factory->Commit();
        
        /* delete added purgeable objects (body, annotations, etc) */
        purge_objects();
    }
}

void DxfExportWorker::handle_body(Body *body)
{
    NXObject *note;
    string note_text;
    double x, y;

    BodyBoundary *bound = new BodyBoundary(body);

    // add thickness annotation if minimum Z is not on XY plane
    if (bound->minimum(BodyBoundary::Z) != 0.0)
        annotations["THICKNESS"] = to_string(bound->thickness());

    // else -> remove thickness annotation
    else
        annotations.erase("THICKNESS");

    // add annotations, if any
    if (annotations.size() > 0)
    {
        x = bound->minimum(BodyBoundary::X) + NOTE_OFFSET;
        y = bound->minimum(BodyBoundary::Y) - NOTE_OFFSET;

        note = add_annotations(x, y);
        add_purgeable_object_to_export(note);
    }
}

bool DxfExportWorker::add_object_to_export(vector<NXObject *> objects)
{
    return dxf_factory->ExportSelectionBlock()->SelectionComp()->Add(objects);
}

bool DxfExportWorker::add_object_to_export(NXObject *object)
{
    return dxf_factory->ExportSelectionBlock()->SelectionComp()->Add(object);
}

bool DxfExportWorker::add_purgeable_object_to_export(NXObject *object)
{
    purgeable_objects.push_back(object);

    return dxf_factory->ExportSelectionBlock()->SelectionComp()->Add(object);
}

void DxfExportWorker::purge_objects()
{
    for (NXObject *object : purgeable_objects)
        dxf_factory->ExportSelectionBlock()->SelectionComp()->Remove(object);
}

bool DxfExportWorker::is_empty_property(string &value)
{
    for (char &c: value)
    {
        switch (c)
        {
            case ' ':
            case 'X':
            case 'x':
                break;
            default:
                return false;
        }
    }

    return true;
}

NXObject *DxfExportWorker::add_annotations(double x_loc, double y_loc)
{
    // switch to drafting
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

    vector<NXString> anno_strings(annotations.size());
    map<string, string>::iterator itr;
    for (itr = annotations.begin(); itr != annotations.end(); ++itr)
        anno_strings.push_back(itr->first + ": " + itr->second);

    note_factory->Text()->TextBlock()->SetText(anno_strings);
    note_factory->Origin()->Plane()->SetPlaneMethod(Annotations::PlaneBuilder::PlaneMethodTypeXyPlane);
    
    // set leader settings
    Annotations::LeaderData *leader_data = part->Annotations()->CreateLeaderData();
    leader_data->SetArrowhead(Annotations::LeaderData::ArrowheadTypeFilledArrow);
    leader_data->SetVerticalAttachment(Annotations::LeaderVerticalAttachmentCenter);
    leader_data->SetStubSide(Annotations::LeaderSideInferred);
    note_factory->Leader()->Leaders()->Append(leader_data);

    // text size and justification
    note_factory->Style()->LetteringStyle()->SetGeneralTextSize(NOTE_SIZE);
    note_factory->Style()->LetteringStyle()->SetHorizontalTextJustification(Annotations::TextJustificationLeft);
    
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
    Point3d note_location(x_loc, y_loc, 0.0);
    note_factory->Origin()->Origin()->SetValue(NULL, note_view, note_location);

    // create note
    NXObject *commit_result = note_factory->Commit();
    note_factory->Destroy();
    
    // switch back to modeling
    nx_session->ApplicationSwitchImmediate("UG_APP_MODELING");

    return commit_result;
}

string DxfExportWorker::get_body_name_by_inference(Body *body)
{
    string body_name;
    vector<Body*> other_bodies;

    BodyBoundary *this_body_bound = new BodyBoundary(body);
    BodyBoundary *other_body_bound;

    for ( Body *other_body: *(part->Bodies()) )
    {
        if (other_body == body)
            continue;

        other_body_bound = new BodyBoundary(other_body);

        other_bodies.push_back(other_body);
    }

    return body_name;
}