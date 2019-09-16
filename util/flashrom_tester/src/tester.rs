//
// Copyright 2019, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Alternatively, this software may be distributed under the terms of the
// GNU General Public License ("GPL") version 2 as published by the Free
// Software Foundation.
//

use super::cmd;
use super::types;
use serde_json::json;

// type-signature comes from the return type of flashrom.rs workers.
type TestError = Box<dyn std::error::Error>;
pub type TestResult = Result<(), TestError>;

type TestFunction = fn(&TestParams) -> TestResult;
type PreFunction = fn(&TestParams) -> ();
type PostFunction = fn(&TestParams) -> ();

pub struct TestParams<'a> {
    pub cmd: &'a cmd::FlashromCmd,
    pub fc: types::FlashChip,
    #[deprecated]
    pub log_text: Option<&'a str>,
    #[deprecated]
    pub pre_fn: Option<PreFunction>,
    #[deprecated]
    pub post_fn: Option<PostFunction>,
}

#[derive(Copy, Clone, PartialEq, Debug)]
pub enum TestConclusion {
    Pass,
    Fail,
    UnexpectedPass,
    UnexpectedFail,
}

pub struct TestCase<'a> {
    pub name: &'a str,
    pub test_fn: TestFunction,
    pub params: &'a TestParams<'a>,
    /// The conclusion returned by this case if `test_fn` returns Ok.
    pub conclusion: TestConclusion,
}

pub struct ReportMetaData {
    pub chip_name: String,
    pub os_release: String,
    pub system_info: String,
    pub bios_info: String,
}

fn decode_test_result(res: TestResult, con: TestConclusion) -> (TestConclusion, Option<TestError>) {
    use TestConclusion::*;

    match (res, con) {
        (Ok(_), Fail) => (UnexpectedPass, None),
        (Err(e), Pass) => (UnexpectedFail, Some(e)),
        _ => (Pass, None),
    }
}

fn run_test(t: &TestCase) -> (TestConclusion, Option<TestError>) {
    let params = &t.params;

    if let Some(msg) = params.log_text {
        info!("{}", msg);
    }

    if params.pre_fn.is_some() {
        params.pre_fn.unwrap()(params);
    }

    let res = (t.test_fn)(params);

    if let Some(f) = params.post_fn {
        f(params);
    }

    decode_test_result(res, t.conclusion)
}

pub fn run_all_tests<'a>(
    ts: &[&TestCase<'a>],
) -> Vec<(&'a str, (TestConclusion, Option<TestError>))> {
    let mut results = Vec::new();
    for t in ts {
        info!("Begin test: {}", t.name);
        let result = run_test(t);
        info!("Completed {}: {:?}", t.name, result);

        results.push((t.name, result));
    }
    results
}

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum OutputFormat {
    Pretty,
    Json,
}

impl std::str::FromStr for OutputFormat {
    type Err = ();

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        use OutputFormat::*;

        if s.eq_ignore_ascii_case("pretty") {
            Ok(Pretty)
        } else if s.eq_ignore_ascii_case("json") {
            Ok(Json)
        } else {
            Err(())
        }
    }
}

pub fn collate_all_test_runs(
    truns: &[(&str, (TestConclusion, Option<TestError>))],
    meta_data: ReportMetaData,
    format: OutputFormat,
) {
    match format {
        OutputFormat::Pretty => {
            println!();
            println!("  =============================");
            println!("  =====  AVL qual RESULTS  ====");
            println!("  =============================");
            println!();
            println!("  %---------------------------%");
            println!("   os release: {}", meta_data.os_release);
            println!("   chip name: {}", meta_data.chip_name);
            println!("   system info: \n{}", meta_data.system_info);
            println!("   bios info: \n{}", meta_data.bios_info);
            println!("  %---------------------------%");
            println!();

            for trun in truns.iter() {
                let (name, (result, error)) = trun;
                if *result != TestConclusion::Pass {
                    println!(
                        " {} {}",
                        style!(format!(" <+> {} test:", name), types::BOLD),
                        style_dbg!(result, types::RED)
                    );
                    match error {
                        None => {}
                        Some(e) => info!(" - {} failure details:\n{}", name, e.to_string()),
                    };
                } else {
                    println!(
                        " {} {}",
                        style!(format!(" <+> {} test:", name), types::BOLD),
                        style_dbg!(result, types::GREEN)
                    );
                }
            }
            println!();
        }
        OutputFormat::Json => {
            use serde_json::{Map, Value};

            let mut all_pass = true;
            let mut tests = Map::<String, Value>::new();
            for (name, (result, error)) in truns {
                let passed = *result == TestConclusion::Pass;
                all_pass &= passed;

                let error = match error {
                    Some(e) => Value::String(format!("{:#?}", e)),
                    None => Value::Null,
                };

                assert!(
                    !tests.contains_key(*name),
                    "Found multiple tests named {:?}",
                    name
                );
                tests.insert(
                    (*name).into(),
                    json!({
                        "pass": passed,
                        "error": error,
                    }),
                );
            }

            let json = json!({
                "pass": all_pass,
                "metadata": {
                    "os_release": meta_data.os_release,
                    "chip_name": meta_data.chip_name,
                    "system_info": meta_data.system_info,
                    "bios_info": meta_data.bios_info,
                },
                "tests": tests,
            });
            println!("{:#}", json);
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::cmd::FlashromCmd;
    use crate::types::FlashChip;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[test]
    fn run_test() {
        use super::TestConclusion::*;
        use super::{run_test, TestCase, TestParams};

        // Hack around TestParams not accepting closures with statics.
        // This is safe for parallel testing because the statics are private
        // to this test case.
        static RAN_PRE: AtomicBool = AtomicBool::new(false);
        static RAN_POST: AtomicBool = AtomicBool::new(false);

        let expected_pass = TestCase {
            name: "ExpectedPass",
            test_fn: |_| Ok(()),
            params: &TestParams {
                cmd: &FlashromCmd {
                    path: "".to_string(),
                    fc: FlashChip::EC,
                },
                fc: FlashChip::HOST,
                log_text: None,
                pre_fn: Some(|_| RAN_PRE.store(true, Ordering::SeqCst)),
                post_fn: Some(|_| RAN_POST.store(true, Ordering::SeqCst)),
            },
            conclusion: Pass,
        };

        let (conclusion, error) = run_test(&expected_pass);
        assert_eq!(conclusion, Pass);
        assert!(error.is_none());
        // Check functions ran and reset flags at the same time
        assert_eq!(RAN_PRE.swap(false, Ordering::SeqCst), true);
        assert_eq!(RAN_POST.swap(false, Ordering::SeqCst), true);

        let unexpected_fail = TestCase {
            test_fn: |_| Err("I'm a failure".into()),
            ..expected_pass
        };
        let (conclusion, error) = run_test(&unexpected_fail);
        assert_eq!(conclusion, UnexpectedFail);
        assert_eq!(format!("{}", error.expect("not an error")), "I'm a failure");

        let expected_fail = TestCase {
            conclusion: Fail,
            ..unexpected_fail
        };
        let (conclusion, error) = run_test(&expected_fail);
        assert_eq!(conclusion, Pass);
        assert!(error.is_none());

        let unexpected_pass = TestCase {
            conclusion: Fail,
            ..expected_pass
        };
        let (conclusion, error) = run_test(&unexpected_pass);
        assert_eq!(conclusion, UnexpectedPass);
        assert!(error.is_none());
    }

    #[test]
    fn output_format_round_trip() {
        use super::OutputFormat::{self, *};

        assert_eq!(format!("{:?}", Pretty).parse::<OutputFormat>(), Ok(Pretty));
        assert_eq!(format!("{:?}", Json).parse::<OutputFormat>(), Ok(Json));
    }
}
