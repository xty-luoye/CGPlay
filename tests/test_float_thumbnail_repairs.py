"""R38/R53: native float EXR-to-OCIO preview; lazy PBO lifecycle."""
from pathlib import Path
import unittest
from tests.native_review_probe import build_and_run
from tests.test_review_metadata_repairs import ROOT,function
OUT=ROOT/'tests/artifacts/fix_review_20261101/stage26'
MAIN=r'''
#include "core/AsyncImageLoader.h"
#include "core/GPUUploadQueue.h"
#include "media/ThumbnailService.h"
#include <QApplication>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLVersionFunctionsFactory>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfHeader.h>
namespace cgplay{
PREVIEW
}
int main(int argc,char**argv){qputenv("QT_QPA_PLATFORM","windows");QApplication app(argc,argv);app.setProperty("cgplay.automationBackground",true);QDir dir(QString::fromUtf8(argv[1]));QJsonObject result;const QString path=dir.filePath("hdr.exr"),config=dir.filePath("gain.ocio");
{Imf::Rgba pixels[3]={Imf::Rgba(-1,-1,-1,1),Imf::Rgba(1,1,1,1),Imf::Rgba(4,4,4,1)};Imf::RgbaOutputFile f(path.toStdString().c_str(),3,1,Imf::WRITE_RGBA);f.setFrameBuffer(pixels,1,3);f.writePixels(1);}
QFile cfg(config);cfg.open(QIODevice::WriteOnly);cfg.write(R"CFG(ocio_profile_version: 2
roles:
  scene_linear: Linear
  default: Linear
file_rules:
  - !<Rule> {name: Default, colorspace: Linear}
displays:
  sRGB:
    - !<View> {name: Gain, colorspace: Display}
active_displays: [sRGB]
active_views: [Gain]
colorspaces:
  - !<ColorSpace>
    name: Linear
    family: ""
    bitdepth: 32f
    isdata: false
    allocation: uniform
  - !<ColorSpace>
    name: Display
    family: ""
    bitdepth: 32f
    isdata: false
    allocation: uniform
    from_scene_reference: !<MatrixTransform> {matrix: [0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, 1]}
)CFG");cfg.close();
cgplay::ImageLoadRequest request;request.filePath=path;auto decoded=cgplay::AsyncImageLoader::loadSync(request);result["realEXRDecodeSucceeded"]=decoded.success;const float*raw=reinterpret_cast<const float*>(decoded.image.constBits());result["sceneLinearRangeRetained"]=decoded.image.format()==QImage::Format_RGBA32FPx4&&raw[0]==-1.f&&raw[4]==1.f&&raw[8]==4.f;
cgplay::OcioManager::PreviewTransformSettings settings;settings.enabled=true;settings.configPath=config;settings.input="Linear";settings.display="sRGB";settings.view="Gain";auto preview=cgplay::ThumbnailService::makePreviewImage(decoded.image,true,QSize(3,1),settings);result["displayTransformBeforeQuantization"]=!preview.isNull()&&preview.pixelColor(0,0).red()==0&&std::abs(preview.pixelColor(1,0).red()-26)<=1&&std::abs(preview.pixelColor(2,0).red()-102)<=1;
settings.enabled=false;auto identity=cgplay::ThumbnailService::makePreviewImage(decoded.image,true,QSize(3,1),settings);result["disabledOCIOIdentityClampsOnlyAtOutput"]=identity.pixelColor(0,0).red()==0&&identity.pixelColor(2,0).red()==255;settings.enabled=true;settings.configPath="missing.ocio";result["invalidConfiguredOCIORejectsPreview"]=cgplay::ThumbnailService::makePreviewImage(decoded.image,true,QSize(3,1),settings).isNull();
QSurfaceFormat format;format.setVersion(4,1);format.setProfile(QSurfaceFormat::CoreProfile);QOpenGLContext context;context.setFormat(format);bool ready=context.create();QOffscreenSurface surface;surface.setFormat(context.format());surface.create();ready=ready&&context.makeCurrent(&surface);auto*gl=ready?QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_1_Core>(&context):nullptr;result["offscreenGLReady"]=gl&&gl->initializeOpenGLFunctions();if(gl){cgplay::GPUUploadQueue queue;queue.initialize(gl);result["initializedQueueDoesNotAllocatePBO"]=queue.isInitialized()&&gl->glIsBuffer(1)==GL_FALSE&&queue.processUploads()==0;cgplay::UploadEntry entry;entry.frame=1;entry.image=QImage(4,4,QImage::Format_RGBA8888);entry.image.fill(Qt::red);GLuint texture=0;queue.setFrameReadyCallback([&](int,GLuint id){texture=id;});queue.enqueue(entry);result["firstUploadAllocatesAndFinishes"]=queue.processUploads()==1&&texture&&queue.pendingCount()==0&&queue.activeCount()==0;GLint size=0;gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER,1);gl->glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER,GL_BUFFER_SIZE,&size);gl->glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);result["storageMatchesRequestedImage"]=size==entry.image.sizeInBytes();queue.shutdown();queue.initialize(gl);queue.enqueue(entry);result["shutdownAndReinitUpload"]=queue.processUploads()==1;queue.shutdown();}
return finish(result);}
'''
class FloatThumbnailRepairs(unittest.TestCase):
 def test_native_pipeline(self):
    source=(ROOT/'src/services/media/ThumbnailService.cpp').read_text(encoding='utf-8-sig');preview=function(source,'QImage ThumbnailService::makePreviewImage(')
    cmake=(ROOT/'tests/artifacts/review_full_20261031/phase24/CMakeLists.txt').read_text(encoding='utf-8-sig')
    extra=cmake[cmake.index('target_include_directories'):].replace('native_probe','probe')
    build_and_run(OUT,MAIN.replace('PREVIEW',preview),['src/common/core/AsyncImageLoader.cpp','src/services/ocio/OcioManager.cpp','src/common/core/GPUUploadQueue.cpp'],extra_cmake=extra)
if __name__=='__main__':unittest.main()
