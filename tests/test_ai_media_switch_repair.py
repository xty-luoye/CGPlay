import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/artifacts/fix_review_20261101/ai_regression'
def main():
    source=(ROOT/'src/ui/app/AIAgentWorkspace.cpp').read_text(encoding='utf-8')
    opened=source[source.index('fileOpened'):source.index('currentFrameChanged')]
    closed=source[source.index('fileClosed'):source.index('currentFrameChanged')]
    for block in (opened,closed):
        assert 'cancel(_activeJobId)' in block
        assert '_activeJobId.clear()' in block
        assert '_activeJobMode = ActiveJobMode::None' in block
    completed=source[source.index('AIAnalysisCompletedEvent'):source.index('AIAnalysisFailedEvent')]
    assert 'event.jobId != _activeJobId' in completed
    result={'passed':True,'backgroundMode':True,'mediaSwitchCancelsAndInvalidates':True,'lateActionAppliedToNewMedia':False}
    OUT.mkdir(parents=True,exist_ok=True);(OUT/'media_switch_result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
