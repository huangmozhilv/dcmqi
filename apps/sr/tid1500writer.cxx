
// DCMTK
#include <dcmtk/config/osconfig.h>   // make sure OS specific configuration is included first
#include <dcmtk/ofstd/ofstream.h>
#include <dcmtk/ofstd/oftest.h>
#include <dcmtk/ofstd/ofstd.h>

#include <dcmtk/dcmsr/dsrdoc.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmiod/modhelp.h>

#include <dcmtk/dcmsr/codes/dcm.h>
#include <dcmtk/dcmsr/codes/srt.h>
#include <dcmtk/dcmsr/cmr/tid1500.h>

#include <dcmtk/dcmdata/dcdeftag.h>

// STD includes
#include <iostream>
#include <exception>

#include <json/json.h>

// DCMQI includes
#include "dcmqi/Exceptions.h"
#include "dcmqi/QIICRConstants.h"
#include "dcmqi/QIICRUIDs.h"
#include "dcmqi/internal/VersionConfigure.h"
#include "dcmqi/Helper.h"

using namespace std;

// not used yet
static OFLogger dcemfinfLogger = OFLog::getLogger("qiicr.apps");

// CLP includes
#undef HAVE_SSTREAM // Avoid redefinition warning
#include "tid1500writerCLP.h"

#define CHECK_BOOL(condition) \
  do { \
    if (!condition) { \
      std::cerr << "Expected True in " __FILE__ << ":" << __LINE__ << " " << std::cout; \
      throw -1; \
    } \
  } while (0);

#define STATIC_ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

DSRCodedEntryValue json2cev(Json::Value& j){
  return DSRCodedEntryValue(j["CodeValue"].asCString(),
    j["CodingSchemeDesignator"].asCString(),
    j["CodeMeaning"].asCString());
}

DcmFileFormat addFileToEvidence(DSRDocument &doc, string dirStr, string fileStr){
  DcmFileFormat ff;
  OFString fullPath;

  if(dirStr.size())
    OFStandard::combineDirAndFilename(fullPath,dirStr.c_str(),fileStr.c_str());
  else
    fullPath = OFString(fileStr.c_str());
  CHECK_COND(ff.loadFile(fullPath));

  CHECK_COND(doc.getCurrentRequestedProcedureEvidence().addItem(*ff.getDataset()));
  return ff;
}


typedef dcmqi::Helper helper;


int main(int argc, char** argv){

  std::cout << dcmqi_INFO << std::endl;

  PARSE_ARGS;

  if(helper::isUndefinedOrPathDoesNotExist(metaDataFileName, "Input metadata file")){
    return EXIT_FAILURE;
  }

  if(outputFileName.empty()) {
    cerr << "Error: Output DICOM file must be specified!" << endl;
    return EXIT_FAILURE;
  }

  Json::Value metaRoot;

  try {
      ifstream metainfoStream(metaDataFileName.c_str(), ifstream::binary);
      metainfoStream >> metaRoot;
  } catch (exception& e) {
      cout << e.what() << '\n';
      return -1;
  }

  TID1500_MeasurementReport report(CMR_CID7021::ImagingMeasurementReport);

  CHECK_COND(report.setLanguage(DSRCodedEntryValue("eng", "RFC5646", "English")));

  /* set details on the observation context */
  string observerType = metaRoot["observerContext"]["ObserverType"].asCString();
  if(observerType == "PERSON"){
    CHECK_COND(report.getObservationContext().addPersonObserver(metaRoot["observerContext"]["PersonObserverName"].asCString(), ""));
  } else if(observerType == "DEVICE"){
    CHECK_COND(report.getObservationContext().addDeviceObserver(metaRoot["observerContext"]["DeviceObserverUID"].asCString()));
  }

  // Image library must be present, even if empty

  CHECK_COND(report.getImageLibrary().createNewImageLibrary());
  CHECK_COND(report.getImageLibrary().addImageGroup());

  if(metaRoot.isMember("imageLibrary")){
    for(Json::ArrayIndex i=0;i<metaRoot["imageLibrary"].size();i++){
      DcmFileFormat ff;
      OFString dicomFilePath;

      // Not sure what is a safe way to combine path components ...
      if(imageLibraryDataDir.size())
        OFStandard::combineDirAndFilename(dicomFilePath,imageLibraryDataDir.c_str(),metaRoot["imageLibrary"][i].asCString());
      else
        dicomFilePath = metaRoot["imageLibrary"][i].asCString();

      cout << "Loading " << dicomFilePath << endl;
      CHECK_COND(ff.loadFile(dicomFilePath));


      if(i==0){
        DcmDataset imageLibGroupDataset;
        const DcmTagKey commonTagsToCopy[] =   {DCM_SOPClassUID,DCM_Modality,DCM_StudyDate,DCM_Columns,DCM_Rows,DCM_PixelSpacing,DCM_BodyPartExamined,DCM_ImageOrientationPatient};
        for(size_t t=0;t<STATIC_ARRAY_SIZE(commonTagsToCopy);t++){
          ff.getDataset()->findAndInsertCopyOfElement(commonTagsToCopy[t],&imageLibGroupDataset);
        }
        CHECK_COND(report.getImageLibrary().addImageEntryDescriptors(imageLibGroupDataset));
      }

      DcmDataset imageEntryDataset;
      const DcmTagKey imageTagsToCopy[] = {DCM_Modality,DCM_SOPClassUID,DCM_SOPInstanceUID,DCM_ImagePositionPatient};
      for(size_t t=0;t<STATIC_ARRAY_SIZE(imageTagsToCopy);t++)
        ff.getDataset()->findAndInsertCopyOfElement(imageTagsToCopy[t],&imageEntryDataset);
      CHECK_COND(report.getImageLibrary().addImageEntry(imageEntryDataset,TID1600_ImageLibrary::withAllDescriptors));

    }
  }

  // TODO
  //  - this is a very narrow procedure code
  // see duscussion here for improved handling, should be factored out in the
  // future, and handled by the upper-level application layers:
  // https://github.com/QIICR/dcmqi/issues/30
  CHECK_COND(report.addProcedureReported(DSRCodedEntryValue("P0-0099A", "SRT", "Imaging procedure")));

  if(!report.isValid()){
    cerr << "Report invalid!" << endl;
    return -1;
  }

  std::cout << "Total measurement groups: " << metaRoot["Measurements"].size() << std::endl;

  for(Json::ArrayIndex i=0;i<metaRoot["Measurements"].size();i++){
    Json::Value measurementGroup = metaRoot["Measurements"][i];

    CHECK_COND(report.addVolumetricROIMeasurements());
    /* fill volumetric ROI measurements with data */
    TID1500_MeasurementReport::TID1411_Measurements &measurements = report.getVolumetricROIMeasurements();
    //std::cout << measurementGroup["TrackingIdentifier"] << std::endl;
    CHECK_COND(measurements.setTrackingIdentifier(measurementGroup["TrackingIdentifier"].asCString()));

    if(metaRoot.isMember("activitySession"))
      CHECK_COND(measurements.setActivitySession(metaRoot["activitySession"].asCString()));
    if(metaRoot.isMember("timePoint"))
      CHECK_COND(measurements.setTimePoint(metaRoot["timePoint"].asCString()));

    if(measurementGroup.isMember("TrackingUniqueIdentifier")) {
      CHECK_COND(measurements.setTrackingUniqueIdentifier(measurementGroup["TrackingUniqueIdentifier"].asCString()));
    } else {
      char uid[100];
      dcmGenerateUniqueIdentifier(uid, QIICR_INSTANCE_UID_ROOT);
      CHECK_COND(measurements.setTrackingUniqueIdentifier(uid));
    }

    CHECK_COND(measurements.setSourceSeriesForSegmentation(measurementGroup["SourceSeriesForImageSegmentation"].asCString()));

    if(measurementGroup.isMember("rwvmMapUsedForMeasurement")){
      CHECK_COND(measurements.setRealWorldValueMap(DSRCompositeReferenceValue(UID_RealWorldValueMappingStorage, measurementGroup["rwvmMapUsedForMeasurement"].asCString())));
    }

    DSRImageReferenceValue segment(UID_SegmentationStorage, measurementGroup["segmentationSOPInstanceUID"].asCString());
    segment.getSegmentList().addItem(measurementGroup["ReferencedSegment"].asInt());
    CHECK_COND(measurements.setReferencedSegment(segment));

    CHECK_COND(measurements.setFinding(json2cev(measurementGroup["Finding"])));
    if(measurementGroup.isMember("FindingSite"))
      CHECK_COND(measurements.addFindingSite(json2cev(measurementGroup["FindingSite"])));

    if(measurementGroup.isMember("MeasurementMethod"))
      CHECK_COND(measurements.setMeasurementMethod(json2cev(measurementGroup["MeasurementMethod"])));

    // TODO - handle conditional items!
    for(Json::ArrayIndex j=0;j<measurementGroup["measurementItems"].size();j++){
      Json::Value measurement = measurementGroup["measurementItems"][j];
      // TODO - add measurement method and derivation!
      const CMR_TID1411_in_TID1500::MeasurementValue numValue(measurement["value"].asCString(), json2cev(measurement["units"]));

      CHECK_COND(measurements.addMeasurement(json2cev(measurement["quantity"]), numValue));

      if(measurement.isMember("derivationModifier")){
          CHECK_COND(measurements.getMeasurement().setDerivation(json2cev(measurement["derivationModifier"])));
      }

      if(measurement.isMember("measurementModifiers"))
        for(Json::ArrayIndex k=0;k<measurement["measurementModifiers"].size();k++)
          CHECK_COND(measurements.getMeasurement().addModifier(json2cev(measurement["measurementModifiers"][k]["modifier"]),json2cev(measurement["measurementModifiers"][k]["modifierValue"])));

      if(measurement.isMember("measurementDerivationParameters")){
        for(Json::ArrayIndex k=0;k<measurement["measurementDerivationParameters"].size();k++){
          Json::Value derivationItem = measurement["measurementDerivationParameters"][k];
          DSRCodedEntryValue derivationParameter =
            json2cev(measurement["measurementDerivationParameters"][k]["derivationParameter"]);

          CMR_SRNumericMeasurementValue derivationParameterValue =
            CMR_SRNumericMeasurementValue(derivationItem["derivationParameterValue"].asCString(),
            json2cev(derivationItem["derivationParameterUnits"]));

          CHECK_COND(measurements.getMeasurement().addDerivationParameter(json2cev(derivationItem["derivationParameter"]), derivationParameterValue));
        }
      }

    }
  }

 if(!report.isValid()){
   cerr << "Report is not valid!" << endl;
   return -1;
 }

  DSRDocument doc;
  OFCondition cond = doc.setTreeFromRootTemplate(report, OFTrue /*expandTree*/);
  if(cond.bad()){
    std::cout << "Failure: " << cond.text() << std::endl;
    return -1;
  }

  // cleanup duplicate modality from image descriptor entry
  //  - if we have any imageLibrary items supplied
  if(metaRoot.isMember("imageLibrary")){
    if(metaRoot["imageLibrary"].size()){
      DSRDocumentTree &st = doc.getTree();
      size_t nnid = st.gotoAnnotatedNode("TID 1601 - Row 1");
      while (nnid) {
        nnid = st.gotoNamedChildNode(CODE_DCM_Modality);
        if (nnid) {
          CHECK_COND(st.removeSubTree());
          nnid = st.gotoNextAnnotatedNode("TID 1601 - Row 1");
        }
      }
    }
  }

  if(metaRoot.isMember("SeriesDescription")) {
    CHECK_COND(doc.setSeriesDescription(metaRoot["SeriesDescription"].asCString()));
  }

  if(metaRoot.isMember("CompletionFlag")) {
    if (DSRTypes::enumeratedValueToCompletionFlag(metaRoot["CompletionFlag"].asCString())
        == DSRTypes::CF_Complete) {
      doc.completeDocument();
    }
  }

  // TODO: we should think about storing those information in json as well
  if(metaRoot.isMember("VerificationFlag") && observerType=="PERSON" && doc.getCompletionFlag() == DSRTypes::CF_Complete) {
    if (DSRTypes::enumeratedValueToVerificationFlag(metaRoot["VerificationFlag"].asCString()) ==
        DSRTypes::VF_Verified) {
      // TODO: get organization from meta information?
      CHECK_COND(doc.verifyDocument(metaRoot["observerContext"]["PersonObserverName"].asCString(), "QIICR"));
    }
  }

  if(metaRoot.isMember("InstanceNumber")) {
    CHECK_COND(doc.setInstanceNumber(metaRoot["InstanceNumber"].asCString()))
  }

  if(metaRoot.isMember("SeriesNumber")) {
    CHECK_COND(doc.setSeriesNumber(metaRoot["SeriesNumber"].asCString()))
  }

  // WARNING: no consistency checks between the referenced UIDs and the
  //  referencedDICOMFileNames ...
  DcmFileFormat ccFileFormat;
  bool compositeContextInitialized = false;
  if(metaRoot.isMember("compositeContext")){
    for(Json::ArrayIndex i=0;i<metaRoot["compositeContext"].size();i++){
      ccFileFormat = addFileToEvidence(doc,compositeContextDataDir,metaRoot["compositeContext"][i].asString());
      compositeContextInitialized = true;
    }
  }

  if(metaRoot.isMember("imageLibrary")){
    for(Json::ArrayIndex i=0;i<metaRoot["imageLibrary"].size();i++){
      addFileToEvidence(doc,imageLibraryDataDir,metaRoot["imageLibrary"][i].asString());
    }
  }

  OFCHECK_EQUAL(doc.getDocumentType(), DSRTypes::DT_EnhancedSR);

  DcmFileFormat ff;
  DcmDataset *dataset = ff.getDataset();

  OFString contentDate, contentTime;
  DcmDate::getCurrentDate(contentDate);
  DcmTime::getCurrentTime(contentTime);

  CHECK_COND(doc.setManufacturer(QIICR_MANUFACTURER));
  CHECK_COND(doc.setDeviceSerialNumber(QIICR_DEVICE_SERIAL_NUMBER));
  CHECK_COND(doc.setManufacturerModelName(QIICR_MANUFACTURER_MODEL_NAME));
  CHECK_COND(doc.setSoftwareVersions(QIICR_SOFTWARE_VERSIONS));

  CHECK_COND(doc.setSeriesDate(contentDate.c_str()));
  CHECK_COND(doc.setSeriesTime(contentTime.c_str()));

  CHECK_COND(doc.write(*dataset));

  if(compositeContextInitialized){
    cout << "Composite Context initialized" << endl;
    DcmModuleHelpers::copyPatientModule(*ccFileFormat.getDataset(),*dataset);
    DcmModuleHelpers::copyPatientStudyModule(*ccFileFormat.getDataset(),*dataset);
    DcmModuleHelpers::copyGeneralStudyModule(*ccFileFormat.getDataset(),*dataset);
  }

  CHECK_COND(ff.saveFile(outputFileName.c_str(), EXS_LittleEndianExplicit));
  std::cout << "SR saved!" << std::endl;

  return 0;
}
