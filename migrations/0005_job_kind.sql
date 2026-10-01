-- 0005: what a job makes. Until now every job made an article. The eval's
-- annotation jobs (rr/eval/annotate.hpp) take a snapshot through other stages,
-- and a job taken up again must run the stages it was started with, so a job
-- says which it is. Jobs from before were all articles.
ALTER TABLE jobs ADD COLUMN kind TEXT NOT NULL DEFAULT 'article' CHECK (kind IN ('article', 'annotation'));
