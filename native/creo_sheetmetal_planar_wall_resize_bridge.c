#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>
#include <ProToolkit.h>
#include <ProMdl.h>
#include <ProSolid.h>
#include <ProFeature.h>
#include <ProModelitem.h>
#include <ProElement.h>
#include <ProSection.h>
#include <ProSecdim.h>
#include <ProSecConstr.h>
#include <ProArray.h>

static int fail(FILE *out, const char *stage, ProError error)
{
    fprintf(out, "{\"ok\":false,\"stage\":\"%s\",\"error_code\":%d}\n", stage, error);
    return 1;
}

static ProError element_by_ids_get(
    ProElement tree, const ProElemId *ids, int count, ProElement *element)
{
    ProElempath path = NULL;
    ProElempathItem items[4];
    ProError status;
    int index;
    for (index = 0; index < count; ++index)
    {
        items[index].type = PRO_ELEM_PATH_ITEM_TYPE_ID;
        items[index].path_item.elem_id = ids[index];
    }
    status = ProElempathAlloc(&path);
    if (status != PRO_TK_NO_ERROR) return status;
    status = ProElempathDataSet(path, items, count);
    if (status == PRO_TK_NO_ERROR)
        status = ProElemtreeElementGet(tree, path, element);
    ProElempathFree(&path);
    return status;
}

static void sort3(double values[3])
{
    double temporary;
    if (values[0] > values[1]) { temporary=values[0]; values[0]=values[1]; values[1]=temporary; }
    if (values[1] > values[2]) { temporary=values[1]; values[1]=values[2]; values[2]=temporary; }
    if (values[0] > values[1]) { temporary=values[0]; values[0]=values[1]; values[1]=temporary; }
}

static int near_value(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

int wmain(int argc, wchar_t **argv)
{
    FILE *out = stdout;
    ProMdl model = NULL;
    ProMdlName model_name;
    ProMdlType model_type;
    ProModelitem profile_item;
    ProFeature profile;
    ProElement working_tree = NULL, rollback_tree = NULL, sketch_element = NULL;
    ProElemId sketch_path[2] = {PRO_E_STD_SECTION, PRO_E_SKETCHER};
    ProSection section = NULL;
    int *entity_ids = NULL, entity_count = 0;
    int *dimension_ids = NULL, dimension_count = 0;
    int *constraint_ids = NULL, constraint_count = 0;
    int old_lines[4] = {-1,-1,-1,-1}, old_line_count = 0, new_lines[4], i;
    double min_x=1e100,max_x=-1e100,min_y=1e100,max_y=-1e100;
    double length, width, thickness;
    ProFeatureCreateOptions *options = NULL;
    ProErrorlist errors = {NULL, 0};
    ProError status;
    int redefined = 0, exit_code = 1;
    ProMassProperty mass;
    Pro3dPnt outline[2];
    ProMatrix matrix = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    ProSolidOutlExclTypes excludes[] = {
        PRO_OUTL_EXC_DATUM_PLANE, PRO_OUTL_EXC_DATUM_POINT,
        PRO_OUTL_EXC_DATUM_CSYS, PRO_OUTL_EXC_DATUM_AXES, PRO_OUTL_EXC_ALL_CRVS};
    double extents[3];

    if (argc != 7) return 2;
    if (_wfopen_s(&out, argv[1], L"wb") != 0 || out == NULL) return 2;
    length = wcstod(argv[5], NULL); width = wcstod(argv[6], NULL);
    if (!(length > 0.0) || !(width > 0.0)) { exit_code=fail(out,"input",PRO_TK_BAD_INPUTS); goto done; }
    status = ProMdlCurrentGet(&model);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"current_model",status); goto done; }
    status = ProMdlNameGet(model, model_name);
    if (status != PRO_TK_NO_ERROR || _wcsicmp(model_name, argv[2]) != 0)
    { exit_code=fail(out,"model_guard",status==PRO_TK_NO_ERROR?PRO_TK_BAD_CONTEXT:status); goto done; }
    status = ProMdlTypeGet(model, &model_type);
    if (status != PRO_TK_NO_ERROR || model_type != PRO_MDL_PART)
    { exit_code=fail(out,"part_guard",status==PRO_TK_NO_ERROR?PRO_TK_INVALID_TYPE:status); goto done; }
    status = ProModelitemByNameInit(model, PRO_FEATURE, argv[3], &profile_item);
    if (status != PRO_TK_NO_ERROR || profile_item.id != _wtoi(argv[4]))
    { exit_code=fail(out,"profile_guard",status==PRO_TK_NO_ERROR?PRO_TK_BAD_CONTEXT:status); goto done; }
    status = ProFeatureInit((ProSolid)model, profile_item.id, &profile);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"profile_init",status); goto done; }
    status = ProFeatureElemtreeExtract(&profile,NULL,PRO_FEAT_EXTRACT_NO_OPTS,&rollback_tree);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"rollback_tree_extract",status); goto done; }
    status = ProFeatureElemtreeExtract(&profile,NULL,PRO_FEAT_EXTRACT_NO_OPTS,&working_tree);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"working_tree_extract",status); goto done; }
    status = element_by_ids_get(working_tree,sketch_path,2,&sketch_element);
    if (status == PRO_TK_NO_ERROR)
        status = ProElementSpecialvalueGet(sketch_element,NULL,(ProAppData *)&section);
    if (status != PRO_TK_NO_ERROR || section == NULL)
    { exit_code=fail(out,"section_get",status==PRO_TK_NO_ERROR?PRO_TK_BAD_CONTEXT:status); goto done; }
    ProSectionIntentManagerModeSet(section, PRO_B_FALSE);
    status = ProSecdimIdsGet(section,&dimension_ids,&dimension_count);
    if (status == PRO_TK_NO_ERROR)
        for (i=0;i<dimension_count;++i) ProSecdimDelete(section,dimension_ids[i]);
    if (dimension_ids != NULL) { ProArrayFree((ProArray *)&dimension_ids); dimension_ids=NULL; }
    status = ProSectionConstraintsIdsGet(section,&constraint_ids,&constraint_count);
    if (status == PRO_TK_NO_ERROR)
    {
        for (i=0;i<constraint_count;++i)
        {
            ProConstraintType type=PRO_CONSTRAINT_TYPE_UNKNOWN; ProConstraintStatus cstatus;
            int count=0,*ids=NULL; ProSectionPointType *senses=NULL;
            if (ProSectionConstraintsGet(section,constraint_ids[i],&type,&cstatus,&count,&ids,&senses)==PRO_TK_NO_ERROR &&
                type==PRO_CONSTRAINT_EQUAL_SEGMENTS)
                ProSectionConstraintDelete(section,constraint_ids[i]);
            if (ids) ProArrayFree((ProArray *)&ids);
            if (senses) ProArrayFree((ProArray *)&senses);
        }
    }
    if (constraint_ids != NULL) { ProArrayFree((ProArray *)&constraint_ids); constraint_ids=NULL; }
    status = ProSectionEntityIdsGet(section,&entity_ids,&entity_count);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"entity_ids",status); goto done; }
    for (i=0;i<entity_count && old_line_count<4;++i)
    {
        ProBoolean projection=PRO_B_FALSE; Pro2dEntdef *entity=NULL;
        if (ProSectionEntityIsProjection(section,entity_ids[i],&projection)!=PRO_TK_NO_ERROR || projection) continue;
        if (ProSectionEntityGet(section,entity_ids[i],&entity)==PRO_TK_NO_ERROR && entity && entity->type==PRO_2D_LINE)
        {
            Pro2dLinedef *line=(Pro2dLinedef *)entity;
            if (fabs(line->end1[0]-line->end2[0]) < 1e-7)
            { double x=(line->end1[0]+line->end2[0])/2.0; if(x<min_x){min_x=x;old_lines[3]=entity_ids[i];} if(x>max_x){max_x=x;old_lines[1]=entity_ids[i];} }
            else if (fabs(line->end1[1]-line->end2[1]) < 1e-7)
            { double y=(line->end1[1]+line->end2[1])/2.0; if(y<min_y){min_y=y;old_lines[0]=entity_ids[i];} if(y>max_y){max_y=y;old_lines[2]=entity_ids[i];} }
            ++old_line_count;
        }
    }
    if (entity_ids != NULL) { ProArrayFree((ProArray *)&entity_ids); entity_ids=NULL; }
    if (old_line_count != 4 || old_lines[0]<0 || old_lines[1]<0 || old_lines[2]<0 || old_lines[3]<0)
    { exit_code=fail(out,"rectangle_guard",PRO_TK_BAD_CONTEXT); goto done; }
    {
        Pro2dLinedef lines[4]; double hx=length/2.0, hy=width/2.0;
        lines[0].type=PRO_2D_LINE; lines[0].end1[0]=-hx; lines[0].end1[1]=-hy; lines[0].end2[0]=hx; lines[0].end2[1]=-hy;
        lines[1].type=PRO_2D_LINE; lines[1].end1[0]=hx; lines[1].end1[1]=-hy; lines[1].end2[0]=hx; lines[1].end2[1]=hy;
        lines[2].type=PRO_2D_LINE; lines[2].end1[0]=hx; lines[2].end1[1]=hy; lines[2].end2[0]=-hx; lines[2].end2[1]=hy;
        lines[3].type=PRO_2D_LINE; lines[3].end1[0]=-hx; lines[3].end1[1]=hy; lines[3].end2[0]=-hx; lines[3].end2[1]=-hy;
        for(i=0;i<4;++i) { status=ProSectionEntityAdd(section,(Pro2dEntdef *)&lines[i],&new_lines[i]); if(status!=PRO_TK_NO_ERROR){exit_code=fail(out,"entity_add",status);goto done;} }
        for(i=0;i<4;++i) { status=ProSectionEntityReplace(section,old_lines[i],new_lines[i]); if(status!=PRO_TK_NO_ERROR){exit_code=fail(out,"entity_replace",status);goto done;} }
    }
    status = ProSectionIntentManagerModeSet(section,PRO_B_TRUE);
    if (status == PRO_TK_NO_ERROR) status = ProSectionIntentManagerModeSet(section,PRO_B_FALSE);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"section_resolve",status); goto done; }
    status = ProArrayAlloc(1,sizeof(ProFeatureCreateOptions),1,(ProArray *)&options);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"options_alloc",status); goto done; }
    options[0]=PRO_FEAT_CR_DEFINE_MISS_ELEMS;
    status = ProFeatureWithoptionsRedefine(NULL,&profile,working_tree,options,PRO_REGEN_NO_FLAGS,&errors);
    if (status != PRO_TK_NO_ERROR || errors.error_number>0)
    { exit_code=fail(out,"profile_redefine",status==PRO_TK_NO_ERROR?PRO_TK_GENERAL_ERROR:status); goto done; }
    redefined=1;
    status = ProSolidRegenerate((ProSolid)model,PRO_REGEN_FORCE_REGEN);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"regenerate",status); goto rollback; }
    status = ProSolidMassPropertyWithDensityGet((ProSolid)model,NULL,PRO_MP_DENS_USE_ALWAYS,1.0,&mass);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"mass",status); goto rollback; }
    status = ProSolidOutlineCompute((ProSolid)model,matrix,excludes,5,outline);
    if (status != PRO_TK_NO_ERROR) { exit_code=fail(out,"outline",status); goto rollback; }
    extents[0]=outline[1][0]-outline[0][0]; extents[1]=outline[1][1]-outline[0][1]; extents[2]=outline[1][2]-outline[0][2]; sort3(extents);
    thickness=extents[0];
    if (!near_value(extents[1],width,1e-5)||!near_value(extents[2],length,1e-5)||!near_value(mass.volume,length*width*thickness,1e-4))
    { exit_code=fail(out,"geometry_verify",PRO_TK_GENERAL_ERROR); goto rollback; }
    status=ProMdlSave(model);
    if(status!=PRO_TK_NO_ERROR){exit_code=fail(out,"save",status);goto rollback;}
    fprintf(out,"{\"ok\":true,\"model\":\"%ls\",\"profile_id\":%d,\"length\":%.9g,\"width\":%.9g,\"thickness\":%.9g,\"volume\":%.9g,\"saved\":true}\n",model_name,profile.id,length,width,thickness,mass.volume);
    exit_code=0; goto done;
rollback:
    if(redefined && rollback_tree){ProErrorlist rollback_errors={NULL,0};ProFeatureWithoptionsRedefine(NULL,&profile,rollback_tree,options,PRO_REGEN_NO_FLAGS,&rollback_errors);ProSolidRegenerate((ProSolid)model,PRO_REGEN_FORCE_REGEN);if(rollback_errors.error_list)ProArrayFree((ProArray *)&rollback_errors.error_list);}
done:
    if(errors.error_list)ProArrayFree((ProArray *)&errors.error_list);
    if(options)ProArrayFree((ProArray *)&options);
    if(working_tree)ProFeatureElemtreeFree(&profile,working_tree);
    if(rollback_tree)ProFeatureElemtreeFree(&profile,rollback_tree);
    if(out && out!=stdout)fclose(out);
    return exit_code;
}
