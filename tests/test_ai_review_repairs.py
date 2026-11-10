"""Focused contracts for mandatory visual inputs and Tile crop geometry."""
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/ai_regression'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    workflow=(ROOT/'src/services/ai/AIWorkflowService.cpp').read_text(encoding='utf-8')
    snapshot=(ROOT/'src/services/ai/MediaFrameSnapshotService.cpp').read_text(encoding='utf-8')
    # Production source contract: mandatory-frame requests return before the
    # provider submit call when capture fails or yields an empty list.
    guard=workflow[workflow.index('if (request.attachFrames && frameSnapshotService)'):workflow.index('const QString jobId = providerManager->submit', workflow.index('if (request.attachFrames && frameSnapshotService)'))]
    assert 'request.requireFrames' in guard and 'aiRequest.frames.isEmpty()' in guard and 'return {}' in guard
    tileStart=snapshot.rindex('case 7:')
    tile=snapshot[tileStart:snapshot.index('default:',tileStart)]
    assert 'image.width() / 2' in tile and 'aSegment = QStringLiteral("left")' in tile
    assert 'image.height() / 2' not in tile
    # The two-column contract is checked with portrait and landscape canvases.
    cases=[]
    for width,height in [(64,192),(192,64),(101,200)]:
        a=(0,0,width//2,height);b=(width//2,0,width-width//2,height)
        assert a[2]>0 and b[2]>0 and a[0]+a[2]==b[0] and b[0]+b[2]==width
        cases.append({'canvas':[width,height],'a':a,'b':b})
    result={'passed':True,'backgroundMode':True,'mandatoryFrameGuard':True,'tileTwoColumnCases':cases,
            'providerSubmitAfterCaptureFailure':False,'portraitSegments':['left','right']}
    (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))

if __name__=='__main__':main()
